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

// I2C and VEML6030 includes
#include "driver/i2c.h"
#include "driver/gpio.h"

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
#define UDP_MESSAGE "This is a test msg"     // Message content
#define UDP_SEND_INTERVAL_MS 3000       // Delay between messages

// VEML6030 I2C Configuration
#define I2C_MASTER_SCL_IO           5    /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           4    /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              0    /*!< I2C master i2c port number, the number of i2c peripheral interfaces available will depend on the chip */
#define I2C_MASTER_FREQ_HZ          400000 /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE   0    /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0    /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000

// VEML6030 Sensor Configuration
#define VEML6030_I2C_ADDR           0x44    /*!< I2C address of VEML6030 */
#define VEML6030_I2C_ADDR_ALT       0x48    /*!< Alternative I2C address of VEML6030 */
#define VEML6030_I2C_ADDR_ALT2      0x60    /*!< Alternative I2C address of VEML6030 */
#define VEML6030_I2C_ADDR_ALT3      0x70    /*!< Alternative I2C address of VEML6030 */
#define VEML6030_REG_ALS_CONF       0x00    /*!< ALS configuration register */
#define VEML6030_REG_ALS            0x04    /*!< ALS data register */
#define VEML6030_REG_WHITE          0x05    /*!< White channel data register */
#define VEML6030_REG_ALS_INT        0x06    /*!< ALS interrupt threshold register */

// VEML6030 Configuration Values
#define VEML6030_ALS_IT_100MS       0x00    /*!< ALS integration time 100ms */
#define VEML6030_ALS_IT_200MS       0x10    /*!< ALS integration time 200ms */
#define VEML6030_ALS_IT_400MS       0x20    /*!< ALS integration time 400ms */
#define VEML6030_ALS_IT_800MS       0x30    /*!< ALS integration time 800ms */
#define VEML6030_ALS_IT_1600MS      0x40    /*!< ALS integration time 1600ms */
#define VEML6030_ALS_IT_3200MS      0x50    /*!< ALS integration time 3200ms */

#define VEML6030_ALS_GAIN_1         0x00    /*!< ALS gain 1x */
#define VEML6030_ALS_GAIN_2         0x04    /*!< ALS gain 2x */
#define VEML6030_ALS_GAIN_4         0x08    /*!< ALS gain 4x */
#define VEML6030_ALS_GAIN_8         0x0C    /*!< ALS gain 8x */

#define VEML6030_ALS_PERS_1         0x00    /*!< ALS persistence 1 */
#define VEML6030_ALS_PERS_2         0x01    /*!< ALS persistence 2 */
#define VEML6030_ALS_PERS_4         0x02    /*!< ALS persistence 4 */
#define VEML6030_ALS_PERS_8         0x03    /*!< ALS persistence 8 */

#define VEML6030_ALS_POWER_ON       0x00    /*!< ALS power on */
#define VEML6030_ALS_POWER_OFF      0x01    /*!< ALS power off */

static otInstance *ot_instance = NULL;  // Global instance pointer
static otUdpSocket udp_socket;          // UDP socket handle (sender)
static bool leader_present = false;     // Flag to indicate leader presence
static otIp6Address leader_addr;        // Leader IPv6 address
static otUdpSocket recv_socket;         // UDP socket handle (receiver)

// Promotion helpers
static bool promotion_task_running = false;

// VEML6030 Sensor variables
static float current_lux = 0.0f;        // Current ambient light reading
static bool sensor_initialized = false; // Sensor initialization status
static SemaphoreHandle_t sensor_mutex;  // Mutex for sensor data access
static uint8_t veml6030_i2c_addr = VEML6030_I2C_ADDR; // Current I2C address being used

// -------- I2C Master Initialization --------
static esp_err_t i2c_master_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C master on SDA=%d, SCL=%d, freq=%d Hz", 
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, I2C_MASTER_FREQ_HZ);
    
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_MASTER_NUM, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        if (ret == ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "Invalid I2C configuration - check GPIO pins and frequency");
        } else if (ret == ESP_ERR_NO_MEM) {
            ESP_LOGE(TAG, "Insufficient memory for I2C driver");
        }
        return ret;
    }

    ESP_LOGI(TAG, "I2C master driver installed successfully");
    
    // Verify I2C driver is working by checking if it can create command links
    i2c_cmd_handle_t verify_cmd = i2c_cmd_link_create();
    if (verify_cmd == NULL) {
        ESP_LOGE(TAG, "Failed to create I2C command link - driver may not be properly installed");
        return ESP_FAIL;
    }
    i2c_cmd_link_delete(verify_cmd);
    ESP_LOGI(TAG, "I2C driver verification successful");
    return ESP_OK;
}

// -------- I2C Scanner Function --------
static esp_err_t i2c_scanner(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus for devices...");
    
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        
        esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, 50 / portTICK_PERIOD_MS);
        i2c_cmd_link_delete(cmd);
        
        if (ret == ESP_OK) {
            if (addr == 0x44 || addr == 0x48 || addr == 0x60 || addr == 0x70) {
                ESP_LOGI(TAG, "I2C device found at address: 0x%02X (potential VEML6030)", addr);
            } else {
                ESP_LOGI(TAG, "I2C device found at address: 0x%02X", addr);
            }
        }
    }
    
    return ESP_OK;
}

// -------- VEML6030 I2C Write Function --------
static esp_err_t veml6030_write_reg(uint8_t reg_addr, uint16_t data)
{
    ESP_LOGD(TAG, "Writing to VEML6030: reg=0x%02X, data=0x%04X", reg_addr, data);
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (veml6030_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write_byte(cmd, data & 0xFF, true);
    i2c_master_write_byte(cmd, (data >> 8) & 0xFF, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

// -------- VEML6030 I2C Read Function --------
static esp_err_t veml6030_read_reg(uint8_t reg_addr, uint16_t *data)
{
    ESP_LOGD(TAG, "Reading from VEML6030: reg=0x%02X", reg_addr);
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (veml6030_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (veml6030_i2c_addr << 1) | I2C_MASTER_READ, true);
    uint8_t byte1, byte2;
    i2c_master_read_byte(cmd, &byte1, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &byte2, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C read failed: %s", esp_err_to_name(ret));
        *data = 0;
    } else {
        *data = (byte2 << 8) | byte1;
        ESP_LOGD(TAG, "Read from VEML6030: reg=0x%02X, data=0x%04X", reg_addr, *data);
    }
    
    return ret;
}

// -------- VEML6030 Address Detection --------
static esp_err_t veml6030_detect_address(void)
{
    ESP_LOGI(TAG, "Detecting VEML6030 I2C address...");
    
    // List of addresses to try (including the ones found by scanner)
    uint8_t addresses_to_try[] = {VEML6030_I2C_ADDR, VEML6030_I2C_ADDR_ALT, VEML6030_I2C_ADDR_ALT2, VEML6030_I2C_ADDR_ALT3};
    const char* address_names[] = {"primary (0x44)", "alt1 (0x48)", "alt2 (0x60)", "alt3 (0x70)"};
    
    for (int i = 0; i < 4; i++) {
        ESP_LOGI(TAG, "Trying VEML6030 at %s...", address_names[i]);
        veml6030_i2c_addr = addresses_to_try[i];
        
        uint16_t test_data = 0;
        esp_err_t ret = veml6030_read_reg(VEML6030_REG_ALS_CONF, &test_data);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "✅ VEML6030 found at address 0x%02X (%s) - ALS_CONF: 0x%04X", 
                     addresses_to_try[i], address_names[i], test_data);
            return ESP_OK;
        } else {
            ESP_LOGD(TAG, "❌ No response at 0x%02X (%s): %s", 
                     addresses_to_try[i], address_names[i], esp_err_to_name(ret));
        }
    }
    
    ESP_LOGE(TAG, "VEML6030 not found at any of the tested addresses");
    return ESP_FAIL;
}

// -------- VEML6030 Sensor Initialization --------
static esp_err_t veml6030_init(void)
{
    esp_err_t ret;
    
    ESP_LOGI(TAG, "Starting VEML6030 sensor initialization...");
    
    // Initialize I2C
    ret = i2c_master_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C master init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "I2C master initialized successfully");

    // Wait a bit for sensor to be ready
    vTaskDelay(pdMS_TO_TICKS(100));

    // Scan I2C bus to see what devices are connected
    i2c_scanner();

    // I2C connectivity is verified by the scanner above

    // Detect the correct VEML6030 I2C address
    ESP_LOGI(TAG, "Attempting to detect VEML6030 at I2C address 0x44...");
    ret = veml6030_detect_address();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VEML6030 address detection failed - trying direct communication test");
        
        // Try direct communication test with address 0x44
        veml6030_i2c_addr = 0x44;
        uint16_t direct_test = 0;
        esp_err_t direct_ret = veml6030_read_reg(VEML6030_REG_ALS_CONF, &direct_test);
        if (direct_ret == ESP_OK) {
            ESP_LOGI(TAG, "Direct communication with 0x44 successful, ALS_CONF: 0x%04X", direct_test);
        } else {
            ESP_LOGE(TAG, "Direct communication with 0x44 failed: %s", esp_err_to_name(direct_ret));
            ESP_LOGE(TAG, "I2C communication error - check wiring and power");
            return ESP_FAIL;
        }
    }
    
    // Verify communication with detected address
    uint16_t test_read = 0;
    ret = veml6030_read_reg(VEML6030_REG_ALS_CONF, &test_read);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VEML6030 communication test failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "VEML6030 communication test successful, current ALS_CONF: 0x%04X", test_read);

    // Configure VEML6030
    // ALS_CONF: Integration time 100ms, gain 1x, persistence 1, power on
    uint16_t als_conf = VEML6030_ALS_IT_100MS | VEML6030_ALS_GAIN_1 | VEML6030_ALS_PERS_1 | VEML6030_ALS_POWER_ON;
    
    ESP_LOGI(TAG, "Writing ALS_CONF register with value: 0x%04X", als_conf);
    ret = veml6030_write_reg(VEML6030_REG_ALS_CONF, als_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VEML6030 configuration write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Verify the configuration was written correctly
    vTaskDelay(pdMS_TO_TICKS(50));
    uint16_t verify_read = 0;
    ret = veml6030_read_reg(VEML6030_REG_ALS_CONF, &verify_read);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "VEML6030 configuration verification read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "VEML6030 configuration verification: 0x%04X", verify_read);

    // Wait for sensor to stabilize after configuration
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "VEML6030 sensor initialized successfully");
    return ESP_OK;
}

// -------- Read Ambient Light from VEML6030 --------
static esp_err_t veml6030_read_lux(float *lux)
{
    uint16_t raw_data;
    esp_err_t ret = veml6030_read_reg(VEML6030_REG_ALS, &raw_data);
    if (ret != ESP_OK) {
        return ret;
    }

    // Convert raw data to lux
    // For gain 1x and integration time 100ms: lux = raw_data * 0.0288
    *lux = raw_data * 0.0288f;
    
    return ESP_OK;
}

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

// -------- Sensor Reading Task (Router Only) --------
static void sensor_reading_task(void *arg)
{
    ESP_LOGI(TAG, "Sensor reading task started for Router");
    
    while (1) {
        otDeviceRole role = otThreadGetDeviceRole(ot_instance);
        
        // Only read sensor data if we're a router
        if (role == OT_DEVICE_ROLE_ROUTER && sensor_initialized) {
            float lux;
            esp_err_t ret = veml6030_read_lux(&lux);
            
            if (ret == ESP_OK) {
                // Update sensor data with mutex protection
                if (xSemaphoreTake(sensor_mutex, portMAX_DELAY) == pdTRUE) {
                    current_lux = lux;
                    xSemaphoreGive(sensor_mutex);
                    ESP_LOGI(TAG, "Ambient light reading: %.2f lux", lux);
                }
            } else {
                ESP_LOGW(TAG, "Failed to read sensor data: %s", esp_err_to_name(ret));
            }
        }
        
        // Read sensor every 2 seconds
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// -------- UDP Sending Task --------
static void udp_send_task(void *arg)
{
    while (1) {
        if (leader_present) {
            otMessage *msg;
            otMessageInfo msgInfo;
            char message_buffer[128];

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

            // Check if we're a router and have sensor data
            otDeviceRole role = otThreadGetDeviceRole(ot_instance);
            if (role == OT_DEVICE_ROLE_ROUTER && sensor_initialized) {
                // Get current sensor reading with mutex protection
                float lux_to_send = 0.0f;
                if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    lux_to_send = current_lux;
                    xSemaphoreGive(sensor_mutex);
                }
                
                // Create sensor data message
                snprintf(message_buffer, sizeof(message_buffer), 
                        "Router Sensor Data: Ambient Light = %.2f lux", lux_to_send);
                ESP_LOGI(TAG, "Sending sensor data to Leader: %.2f lux", lux_to_send);
            } else {
                // Send default message for non-router or when sensor not available
                snprintf(message_buffer, sizeof(message_buffer), UDP_MESSAGE);
            }

            otMessageAppend(msg, message_buffer, strlen(message_buffer));

            // Send
            otError error = otUdpSend(ot_instance, &udp_socket, msg, &msgInfo);
            if (error == OT_ERROR_NONE) {
                ESP_LOGI(TAG, "UDP message sent to Leader: %s", message_buffer);
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

                // Initialize VEML6030 sensor for router
                if (!sensor_initialized) {
                    ESP_LOGI(TAG, "Attempting to initialize VEML6030 sensor for Router...");
                    esp_err_t sensor_ret = veml6030_init();
                    if (sensor_ret == ESP_OK) {
                        sensor_initialized = true;
                        ESP_LOGI(TAG, "VEML6030 sensor initialized successfully for Router");
                        
                        // Create sensor reading task
                        BaseType_t task_ret = xTaskCreate(sensor_reading_task, "sensor_task", 4096, NULL, 3, NULL);
                        if (task_ret == pdPASS) {
                            ESP_LOGI(TAG, "Sensor reading task created successfully");
                        } else {
                            ESP_LOGE(TAG, "Failed to create sensor reading task");
                        }
                    } else {
                        ESP_LOGW(TAG, "VEML6030 sensor initialization failed: %s", esp_err_to_name(sensor_ret));
                        ESP_LOGW(TAG, "Router will continue without sensor functionality");
                        ESP_LOGW(TAG, "Please check: 1) VEML6030 is connected to GPIO4(SDA) and GPIO5(SCL)");
                        ESP_LOGW(TAG, "2) VEML6030 is powered (3.3V) - found devices at 0x60, 0x70");
                        ESP_LOGW(TAG, "3) Pull-up resistors are present on SDA/SCL lines");
                    }
                }

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

    // Create sensor mutex
    sensor_mutex = xSemaphoreCreateMutex();
    if (sensor_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor mutex");
    }

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
