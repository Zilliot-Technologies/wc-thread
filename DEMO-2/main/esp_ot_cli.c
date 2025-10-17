#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_types.h"
#include "esp_ot_config.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "openthread/instance.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "openthread/tasklet.h"
#include "openthread/platform/logging.h"
#include "openthread/ip6.h"
#include "openthread/udp.h"

#include <stdarg.h>

#define TAG "ot_api_node"
#define OT_LOG_TAG "OPENTHREAD"

// UDP settings
#define UDP_PORT 12345                  // UDP destination port
#define UDP_MESSAGE "Hello Leader!"     // Message content
#define UDP_SEND_INTERVAL_MS 3000       // Delay between messages

static otInstance *ot_instance = NULL;  // Global instance pointer
static otUdpSocket udp_socket;          // UDP socket handle (sender)
static bool leader_present = false;     // Flag to indicate leader presence
static otIp6Address leader_addr;        // Leader IPv6 address
static otUdpSocket recv_socket;         // UDP socket handle (receiver)

// Promotion helpers
static bool promotion_task_running = false;

// -------- Logging wrapper for OpenThread --------
void otPlatLog(otLogLevel aLogLevel, otLogRegion aLogRegion, const char *aFormat, ...)
{
    if (aLogLevel == OT_LOG_LEVEL_WARN || aLogLevel == OT_LOG_LEVEL_CRIT) {
        va_list args;
        va_start(args, aFormat);

        if (aLogLevel == OT_LOG_LEVEL_CRIT) {
            esp_log_level_set(OT_LOG_TAG, ESP_LOG_ERROR);
            esp_log_writev(ESP_LOG_ERROR, OT_LOG_TAG, aFormat, args);
        } else if (aLogLevel == OT_LOG_LEVEL_WARN) {
            esp_log_level_set(OT_LOG_TAG, ESP_LOG_WARN);
            esp_log_writev(ESP_LOG_WARN, OT_LOG_TAG, aFormat, args);
        }

        va_end(args);
    }
}

// -------- Initialize OpenThread netif --------
static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif != NULL);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));
    return netif;
}

// -------- Promotion task: repeatedly try to become Router --------
static void router_promotion_task(void *arg)
{
    const TickType_t try_delay = pdMS_TO_TICKS(2000); // 2s between attempts
    const int max_attempts = 15; // ~30 seconds
    int attempts = 0;

    ESP_LOGI(TAG, "Router promotion task started");

    while (attempts < max_attempts) {
        otDeviceRole role = otThreadGetDeviceRole(ot_instance);
        if (role == OT_DEVICE_ROLE_ROUTER) {
            ESP_LOGI(TAG, "Promotion succeeded (role = Router)");
            break;
        }

        // Ensure router-eligible and friendly thresholds
        otThreadSetRouterEligible(ot_instance, true);
        otThreadSetRouterUpgradeThreshold(ot_instance, 1);
        otThreadSetRouterDowngradeThreshold(ot_instance, 0);

        otError err = otThreadBecomeRouter(ot_instance);
        if (err == OT_ERROR_NONE) {
            ESP_LOGI(TAG, "Requested router role (attempt %d)", attempts + 1);
        } else {
            ESP_LOGW(TAG, "otThreadBecomeRouter returned %d (attempt %d)", err, attempts + 1);
        }

        attempts++;
        vTaskDelay(try_delay);
    }

    if (attempts >= max_attempts) {
        ESP_LOGW(TAG, "Router promotion task timed out after %d attempts", attempts);
    }

    promotion_task_running = false;
    vTaskDelete(NULL);
}

// -------- UDP Sending Task --------
static void udp_send_task(void *arg)
{
    while (1) {
        if (leader_present) {
            otMessage *msg;
            otMessageInfo msgInfo;

            memset(&msgInfo, 0, sizeof(msgInfo));
            msgInfo.mPeerAddr = leader_addr;  // Leader's IPv6 address (RLOC/EID depending on API)
            msgInfo.mPeerPort = UDP_PORT;

            // Create a new UDP message
            msg = otUdpNewMessage(ot_instance, NULL);
            if (msg == NULL) {
                ESP_LOGE(TAG, "Failed to allocate UDP message");
                vTaskDelay(pdMS_TO_TICKS(UDP_SEND_INTERVAL_MS));
                continue;
            }

            otMessageAppend(msg, UDP_MESSAGE, strlen(UDP_MESSAGE));

            // Send
            otError error = otUdpSend(ot_instance, &udp_socket, msg, &msgInfo);
            if (error == OT_ERROR_NONE) {
                ESP_LOGI(TAG, "UDP message sent to Leader: %s", UDP_MESSAGE);
            } else {
                ESP_LOGE(TAG, "UDP send failed: %d", error);
                otMessageFree(msg);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(UDP_SEND_INTERVAL_MS)); // 3 seconds delay
    }
}

static void udp_receive_cb(void *aContext, otMessage *aMessage,
                           const otMessageInfo *aMessageInfo)
{
    char buf[128];
    char ip_str[OT_IP6_ADDRESS_STRING_SIZE];
    int len = otMessageRead(aMessage, 0, buf, sizeof(buf) - 1);
    if (len < 0) len = 0;
    if (len > (int)sizeof(buf) - 1) len = sizeof(buf) - 1;
    buf[len] = '\0';

    otIp6AddressToString(&aMessageInfo->mPeerAddr, ip_str, sizeof(ip_str));

    ESP_LOGI("UDP_RECV", "Received from [%s]:%d - %s",
            ip_str,
            aMessageInfo->mPeerPort,
            buf);
}

static void start_udp_listener(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otSockAddr bindAddr;

    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.mPort = UDP_PORT;  // same as router's UDP_PORT

    otError err;

    err = otUdpOpen(instance, &recv_socket, udp_receive_cb, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otUdpOpen (recv) failed: %d", err);
        return;
    }

    // Bind to Thread netif. Some ESP-IDF versions use OT_NETIF_THREAD_HOST
    #ifdef OT_NETIF_THREAD_HOST
        err = otUdpBind(instance, &recv_socket, &bindAddr, OT_NETIF_THREAD_HOST);
    #else
        err = otUdpBind(instance, &recv_socket, &bindAddr, OT_NETIF_THREAD_HOST);
    #endif

    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otUdpBind (recv) failed: %d", err);
        // continue even if bind fails
    } else {
        ESP_LOGI(TAG, "UDP listener bound to port %d", UDP_PORT);
    }
}

// -------- Role Change Callback --------
static void on_thread_state_changed(otChangedFlags flags, void *context)
{
    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole role = otThreadGetDeviceRole(ot_instance);

        switch (role) {
            case OT_DEVICE_ROLE_CHILD:
                ESP_LOGI(TAG, "📡 Attached as Child — requesting Router role...");
                otThreadBecomeRouter(ot_instance);
                break;

            case OT_DEVICE_ROLE_ROUTER:
                ESP_LOGI(TAG, "🎯 Thread role: Router");

                if (otThreadGetLeaderRloc(ot_instance, &leader_addr) == OT_ERROR_NONE) {
                    char addr_str[OT_IP6_ADDRESS_STRING_SIZE];
                    otIp6AddressToString(&leader_addr, addr_str, sizeof(addr_str));
                    ESP_LOGI(TAG, "Leader Mesh-Local EID: %s", addr_str);
                    leader_present = true;
                } else {
                    ESP_LOGW(TAG, "No Leader EID found.");
                    leader_present = false;
                }
                break;

            case OT_DEVICE_ROLE_LEADER:
                ESP_LOGI(TAG, "👑 Thread role: Leader");
                leader_present = false;
                break;

            default:
                ESP_LOGI(TAG, "Thread role: %d", role);
                leader_present = false;
                break;
        }
    }
}


// -------- Main OpenThread Worker Task --------
static void ot_task_worker(void *aContext)
{
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    ESP_ERROR_CHECK(esp_openthread_init(&config));

    esp_netif_t *openthread_netif = init_openthread_netif(&config);
    esp_netif_set_default_netif(openthread_netif);

    ot_instance = esp_openthread_get_instance();

    otSetStateChangedCallback(ot_instance, on_thread_state_changed, NULL);

    // Dataset configuration
    if (!otDatasetIsCommissioned(ot_instance)) {
        ESP_LOGI(TAG, "Dataset not commissioned, applying custom dataset...");
        otOperationalDataset dataset = {0};

        static const uint8_t networkKey[16] = {
            0xf3, 0x11, 0x44, 0x55, 0x97, 0x26, 0x0b, 0x5f,
            0x92, 0x28, 0xb6, 0xa5, 0xcc, 0x6f, 0x5f, 0xa7
        };

        static const char networkName[] = "OpenThread-dabd";
        static const uint8_t extPanId[8] = {
            0x0c, 0xc7, 0x76, 0xe5, 0xb8, 0x0d, 0xaa, 0x38
        };

        dataset.mActiveTimestamp.mSeconds = 1;
        dataset.mChannel = 17;
        dataset.mPanId = 0xdabd;

        memcpy(dataset.mNetworkKey.m8, networkKey, sizeof(networkKey));
        memcpy(dataset.mNetworkName.m8, networkName, strlen(networkName) + 1);
        memcpy(dataset.mExtendedPanId.m8, extPanId, sizeof(extPanId));

        dataset.mComponents.mIsActiveTimestampPresent = true;
        dataset.mComponents.mIsChannelPresent = true;
        dataset.mComponents.mIsPanIdPresent = true;
        dataset.mComponents.mIsNetworkKeyPresent = true;
        dataset.mComponents.mIsNetworkNamePresent = true;
        dataset.mComponents.mIsExtendedPanIdPresent = true;

        otOperationalDatasetTlvs tlvs;
        otDatasetConvertToTlvs(&dataset, &tlvs);
        ESP_ERROR_CHECK(otDatasetSetActiveTlvs(ot_instance, &tlvs));
    }

    // Link mode configuration
    otLinkModeConfig mode = {
        .mRxOnWhenIdle = true,
        .mDeviceType = true,
        .mNetworkData = true
    };
    otThreadSetLinkMode(ot_instance, mode);

    // Force router eligibility and aggressive thresholds
    otThreadSetRouterEligible(ot_instance, true);
    otThreadSetRouterUpgradeThreshold(ot_instance, 1);
    otThreadSetRouterDowngradeThreshold(ot_instance, 0);

    // Enable IPv6 and Thread
    otIp6SetEnabled(ot_instance, true);
    ESP_ERROR_CHECK(otThreadSetEnabled(ot_instance, true));

    // Start UDP listener (runs for both Leader and Router)
    start_udp_listener();

    // Open UDP socket for sending
    otError err = otUdpOpen(ot_instance, &udp_socket, NULL, NULL);
    if (err != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otUdpOpen (send) failed: %d", err);
    } else {
        ESP_LOGI(TAG, "UDP send socket opened");
    }

    // Start UDP send task (it will only send when leader_present == true)
    xTaskCreate(udp_send_task, "udp_send_task", 4096, NULL, 4, NULL);

    // Run OpenThread main loop (never returns)
    esp_openthread_launch_mainloop();

    // Cleanup (never reached)
    otUdpClose(ot_instance, &udp_socket);
    esp_openthread_netif_glue_deinit();
    esp_netif_destroy(openthread_netif);
    esp_vfs_eventfd_unregister();
    vTaskDelete(NULL);
}

void app_main(void)
{
    // NOTE: nvs_flash_erase() will erase stored Thread creds; keep only for fresh tests
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 3};

    ESP_LOGI(TAG, "Initializing Thread device (API-based, no CLI)");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));

    xTaskCreate(ot_task_worker, "ot_api_main", 10240, NULL, 5, NULL);
}
