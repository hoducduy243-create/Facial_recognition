#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "SLAVE_ESP32CAM";

// ==============================================================================
// 1. CẤU HÌNH WIFI & SERVER
// ==============================================================================
#define WIFI_SSID     "hutea"
#define WIFI_PASS     "huteacamonban"
#define SERVER_URL "http://192.168.1.106:8000/api/upload_slave"    
// Chân Camera AI-Thinker ESP32-CAM (gắn OV3660)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

enum MessageType {
    MSG_CMD_CAPTURE   = 1,
    MSG_ACK_RECEIVED  = 2,
    MSG_ACK_UPLOADED  = 3
};
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;
    char     session_id[32];
} ControlMessage;

static uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static char active_session_id[32] = "";
static volatile bool capture_flag = false;
// Danh dau Slave dang upload anh
static volatile bool slave_uploading = false;
static esp_err_t init_camera_ov3660(void)
{
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_SVGA; // 640x480
    config.jpeg_quality = 9;
    config.fb_count     = 1;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi khởi tạo Camera OV3660: 0x%x", err);
            return err;
    }

   sensor_t *s = esp_camera_sensor_get();
if (s != NULL)
{
    /* Xoay anh 180 do */
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);

    /* Can chinh chat luong anh */
    s->set_brightness(s, 0);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);

    /* Tu dong dieu chinh anh sang */
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);

    /* Sua diem anh loi + lens correction */
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
    s->set_lenc(s, 1);
}

ESP_LOGI(TAG, "Camera da khoi tao va toi uu");
    return ESP_OK;
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Đang bắt đầu kết nối tới SSID: %s ...", WIFI_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* disconn = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "WiFi mất kết nối! Mã lỗi lý do (Reason Code): %d", disconn->reason);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, ">>> ĐÃ KẾT NỐI WIFI THÀNH CÔNG! IP: " IPSTR " <<<", IP2STR(&event->ip_info.ip));
    }
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}
static void send_ack(uint8_t ack_type, const char *session_id)
{
    ControlMessage ack = {0};

    ack.msg_type = ack_type;

    strncpy(
        ack.session_id,
        session_id,
        sizeof(ack.session_id) - 1
    );

    ack.session_id[
        sizeof(ack.session_id) - 1
    ] = '\0';

    esp_err_t err = esp_now_send(
        broadcastAddress,
        (uint8_t *)&ack,
        sizeof(ControlMessage)
    );

    if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "[ESP-NOW] Gui ACK that bai: %s",
            esp_err_to_name(err)
        );
    }
}
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_esp_now_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
#else
static void on_esp_now_recv(const uint8_t *mac_addr, const uint8_t *data, int len)
#endif
{
    if (len != sizeof(ControlMessage)) return;
    ControlMessage *msg = (ControlMessage *)data;

    if (msg->msg_type == MSG_CMD_CAPTURE) {

    strncpy(
        active_session_id,
        msg->session_id,
        sizeof(active_session_id) - 1
    );

    active_session_id[
        sizeof(active_session_id) - 1
    ] = '\0';

    /* Xác nhận với Master rằng Slave đã nhận lệnh */
    send_ack(
        MSG_ACK_RECEIVED,
        active_session_id
    );

    capture_flag = true;

    ESP_LOGI(
        TAG,
        "[ESP-NOW] Nhan lenh CAPTURE - Session: %s",
        active_session_id
    );
}
}

/* =========================================================
 * SLAVE HEARTBEAT TASK
 * ========================================================= */

#define SLAVE_HEARTBEAT_URL "http://192.168.1.106:8000/api/heartbeat/slave"

static void slave_heartbeat_task(void *pvParameters)
{
    while (1) {
        // Uu tien chup va upload anh Slave
        if (slave_uploading) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }   

        esp_http_client_config_t config = {
            .url = SLAVE_HEARTBEAT_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        if (client != NULL) {

            esp_err_t err = esp_http_client_perform(client);

            if (err == ESP_OK) {

                int status_code = esp_http_client_get_status_code(client);

                if (status_code == 200) {
                    ESP_LOGI(TAG, "[HEARTBEAT] Slave ONLINE");
                } else {
                    ESP_LOGW(
                        TAG,
                        "[HEARTBEAT] Server HTTP %d",
                        status_code
                    );
                }

            } else {

                ESP_LOGW(
                    TAG,
                    "[HEARTBEAT] Khong ket noi duoc Server: %s",
                    esp_err_to_name(err)
                );
            }

            esp_http_client_cleanup(client);
        }

        // Gui heartbeat moi 5 giay
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
static void upload_image_task(void *pvParameters)
{
    while (1) {
        if (capture_flag) {
            capture_flag = false;
            // Bat dau qua trinh chup va upload anh
            slave_uploading = true;

            // Lấy và bỏ frame đầu
camera_fb_t *fb = esp_camera_fb_get();

if (fb) {
    esp_camera_fb_return(fb);
}

// Chờ AWB / Exposure ổn định
vTaskDelay(pdMS_TO_TICKS(300));

// Lấy frame chính thức để gửi Server
fb = esp_camera_fb_get();

if (!fb) {
    ESP_LOGE(TAG, "Không thể chụp ảnh từ OV3660!");
    slave_uploading = false;
    continue;
}

ESP_LOGI(TAG,
         "Slave captured: %dx%d, JPEG = %u bytes",
         fb->width,
         fb->height,
         fb->len);

            char url_with_session[256];
            snprintf(url_with_session, sizeof(url_with_session), "%s?session_id=%s", SERVER_URL, active_session_id);

            esp_http_client_config_t http_cfg = {
                .url = url_with_session,
                .method = HTTP_METHOD_POST,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = 30000,
                .skip_cert_common_name_check = true,
                .buffer_size_tx = 4096,
            };
            esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
            esp_http_client_set_header(client, "Content-Type", "image/jpeg");
            esp_http_client_set_post_field(client, (const char *)fb->buf, fb->len);

            esp_err_t err = esp_http_client_perform(client);
            int http_status = esp_http_client_get_status_code(client);

if (err == ESP_OK &&
    http_status >= 200 &&
    http_status < 300)
{
    ESP_LOGI(
        TAG,
        "Gui anh Slave thanh cong! HTTP Status = %d",
        http_status
    );

    /* Bao cho Master: Slave da upload anh thanh cong */
    send_ack(
        MSG_ACK_UPLOADED,
        active_session_id
    );

    ESP_LOGI(
        TAG,
        "[ESP-NOW] Da gui ACK_UPLOADED - Session: %s",
        active_session_id
    );
}
else
{
    ESP_LOGE(
        TAG,
        "Upload Slave that bai! ESP=%s, HTTP=%d",
        esp_err_to_name(err),
        http_status
    );
}
            esp_http_client_cleanup(client);
            esp_camera_fb_return(fb);
            // Ket thuc qua trinh upload anh
            slave_uploading = false;

        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}


void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    ESP_LOGI(TAG, "Khởi động Slave ESP32-CAM...");
    ESP_ERROR_CHECK(init_camera_ov3660());
    init_wifi();

    // Khởi tạo ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peerInfo));
    ESP_ERROR_CHECK(esp_now_r   egister_recv_cb(on_esp_now_recv));
    // Khoi dong Heartbeat Task cho Slave
xTaskCreate(
    slave_heartbeat_task,
    "slave_heartbeat",
    4096,
    NULL,
    3,
    NULL
);
    



    xTaskCreate(upload_image_task, "upload_task", 8192, NULL, 5, NULL);
}
