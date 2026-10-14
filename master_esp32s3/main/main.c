#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_camera.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

#include "driver/gpio.h"
#include "jpeg_decoder.h"
#include "display_st7789.h"

static const char *TAG = "MASTER_ESP32S3";

/* =========================================================
 * WIFI + SERVER
 * ========================================================= */
#define WIFI_SSID     "hutea"
#define WIFI_PASS     "huteacamonban"
#define SERVER_URL    "http://192.168.1.106:8000/api/upload_master"

/* =========================================================
 * GPIO
 * ========================================================= */
#define RELAY_PIN 1
#define PIR_PIN GPIO_NUM_14

/* =========================================================
 * CAMERA OV5640
 * ========================================================= */
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1

#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM      4
#define SIOC_GPIO_NUM      5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM        8
#define Y3_GPIO_NUM        9
#define Y2_GPIO_NUM       11

#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13

/* =========================================================
 * ESP-NOW
 * ========================================================= */
enum MessageType {
    MSG_CMD_CAPTURE   = 1,
    MSG_ACK_RECEIVED  = 2,
    MSG_ACK_UPLOADED  = 3
};

typedef struct __attribute__((packed)) {
    uint8_t msg_type;
    char session_id[32];
} ControlMessage;

/* Broadcast */
static uint8_t broadcastAddress[] = {
    0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF
};

/* =========================================================
 * GLOBAL VARIABLES
 * ========================================================= */
static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *lcd_render_buf = NULL;

static volatile bool slave_ack_received = false;
static volatile bool slave_upload_done = false;
static char current_session_id[32] = "";
static char http_response_buffer[1024] = "";

#define SCREEN_TIMEOUT_US (5ULL * 60ULL * 1000000ULL) // 5 phút đếm ngược

/* =========================================================
 * CAMERA INIT
 * ========================================================= */
static esp_err_t init_camera_ov5640(void)
{
    camera_config_t config = {0};

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;

    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;

    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href  = HREF_GPIO_NUM;

    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;

    config.pin_pwdn  = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;

    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_240X240; // Chuẩn 240x240 hiển thị trực tiếp 1:1 lên ST7789
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Lỗi khởi tạo Camera OV5640: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
        s->set_brightness(s, 0);
        s->set_contrast(s, 1);
        s->set_saturation(s, 0);
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_exposure_ctrl(s, 1);
        s->set_gain_ctrl(s, 1);
        s->set_bpc(s, 1);
        s->set_wpc(s, 1);
        s->set_lenc(s, 1);
    }

    ESP_LOGI(TAG, "Camera OV5640 đã khởi tạo chuẩn 240x240 và tối ưu");
    return ESP_OK;
}

/* =========================================================
 * WIFI EVENT
 * ========================================================= */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Master đã kết nối WiFi! IP: " IPSTR, IP2STR(&event->ip_info.ip));
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

/* =========================================================
 * ESP-NOW CALLBACK
 * ========================================================= */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static void on_esp_now_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
#else
static void on_esp_now_recv(const uint8_t *mac_addr, const uint8_t *data, int len)
#endif
{
    if (len != sizeof(ControlMessage)) return;

    const ControlMessage *msg = (const ControlMessage *)data;
    if (strncmp(msg->session_id, current_session_id, sizeof(current_session_id)) != 0) {
        return;
    }

    if (msg->msg_type == MSG_ACK_RECEIVED) {
        slave_ack_received = true;
        ESP_LOGI(TAG, "[SLAVE ACK] Slave đã nhận lệnh CAPTURE - Session: %s", msg->session_id);
    } else if (msg->msg_type == MSG_ACK_UPLOADED) {
        slave_upload_done = true;
        ESP_LOGI(TAG, "[SLAVE ACK] Slave upload thành công - Session: %s", msg->session_id);
    }
}

/* =========================================================
 * HTTP EVENT HANDLER
 * ========================================================= */
/* =========================================================
 * MASTER HEARTBEAT TASK
 * ========================================================= */

#define HEARTBEAT_URL "http://192.168.1.106:8000/api/heartbeat/master"

static void master_heartbeat_task(void *pvParameters)
{
    while (1) {

        esp_http_client_config_t config = {
            .url = HEARTBEAT_URL,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 3000
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);

        if (client != NULL) {

            esp_err_t err = esp_http_client_perform(client);

            if (err == ESP_OK) {
                int status_code = esp_http_client_get_status_code(client);

                if (status_code == 200) {
                    ESP_LOGI(TAG, "[HEARTBEAT] Master ONLINE");
                } else {
                    ESP_LOGW(TAG, "[HEARTBEAT] Server HTTP %d", status_code);
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

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (!esp_http_client_is_chunked_response(evt->client)) {
            int copy_len = evt->data_len < (int)sizeof(http_response_buffer) - 1 ?
                           evt->data_len : (int)sizeof(http_response_buffer) - 1;
            memcpy(http_response_buffer, evt->data, copy_len);
            http_response_buffer[copy_len] = '\0';
        }
    }
    return ESP_OK;
}

/* =========================================================
 * HARDWARE INIT (PIR & RELAY)
 * ========================================================= */
static void init_hardware(void)
{
    // PIR HC-SR501
    gpio_config_t pir_config = {
        .pin_bit_mask = (1ULL << PIR_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pir_config));

    // Relay GPIO
    gpio_config_t relay_cfg = {
        .pin_bit_mask = (1ULL << RELAY_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&relay_cfg));
    gpio_set_level(RELAY_PIN, 0);

    ESP_LOGI(TAG, "Khởi tạo PIR (GPIO%d) và Relay (GPIO%d) thành công", PIR_PIN, RELAY_PIN);
}

/* =========================================================
 * RENDER CAMERA PREVIEW TO LCD
 * ========================================================= */
static bool decode_and_render_preview(camera_fb_t *fb, int remaining_sec, const char *status_msg)
{
    if (!fb || !lcd_render_buf || !panel_handle) {
        return false;
    }

    esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = fb->buf,
        .indata_size = fb->len,
        .outbuf = (uint8_t *)lcd_render_buf,
        .outbuf_size = LCD_H_RES * LCD_V_RES * sizeof(uint16_t),
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {
            .swap_color_bytes = 1,
        }
    };
    esp_jpeg_image_output_t out_img;
    esp_err_t ret = esp_jpeg_decode(&jpeg_cfg, &out_img);
    if (ret != ESP_OK) {
        return false;
    }

    st7789_draw_face_overlay(lcd_render_buf, LCD_H_RES, LCD_V_RES, remaining_sec, status_msg);
    st7789_draw_frame(panel_handle, 0, 0, LCD_H_RES, LCD_V_RES, lcd_render_buf);
    return true;
}

/* =========================================================
 * ACCESS CONTROL TASK
 * ========================================================= */
static void access_control_task(void *pvParameters)
{
    int64_t screen_off_deadline_us = 0;
    bool screen_is_on = false;

    // Ban đầu tắt màn hình
    st7789_set_backlight(false);
    esp_lcd_panel_disp_on_off(panel_handle, false);
    st7789_fill_color(panel_handle, COLOR_BLACK);

    ESP_LOGI(TAG, "Access Control Task sẵn sàng. Chờ PIR phát hiện chuyển động...");

    while (1) {
        bool pir_detected = (gpio_get_level(PIR_PIN) == 1);

        if (pir_detected) {
            // Khi PIR phát hiện người: Reset thời gian sáng màn hình thêm 5 phút
            screen_off_deadline_us = esp_timer_get_time() + SCREEN_TIMEOUT_US;

            if (!screen_is_on) {
                ESP_LOGI(TAG, ">>> [PIR] Phát hiện người -> BẬT MÀN HÌNH TFT (Bộ đếm: 5 phút) <<<");
                screen_is_on = true;
                esp_lcd_panel_disp_on_off(panel_handle, true);
                st7789_set_backlight(true);
            } else {
                ESP_LOGI(TAG, "[PIR] Quét thấy chuyển động tiếp -> Reset lại bộ đếm 5 phút");
            }

            // =========================================================
            // BƯỚC 1: HIỆN THÔNG BÁO CHÀO MỪNG TRONG 2.5 GIÂY
            // "Xin Chào. Vui lòng đặt khuôn mặt vào khung trong vòng 20s"
            // =========================================================
            ESP_LOGI(TAG, "[UI] Hiển thị thông điệp Chào Mừng & Hướng dẫn 20s...");
            st7789_show_welcome_screen(panel_handle, lcd_render_buf);

            for (int w = 0; w < 25; w++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                if (gpio_get_level(PIR_PIN) == 1) {
                    screen_off_deadline_us = esp_timer_get_time() + SCREEN_TIMEOUT_US;
                }
            }

            // =========================================================
            // BƯỚC 2: MỞ CAMERA LIVE PREVIEW TRÊN TFT & ĐẾM NGƯỢC 20S
            // =========================================================
            ESP_LOGI(TAG, "[LIVE PREVIEW] Mở camera live trên ST7789 & đếm ngược 20s...");
            int64_t countdown_end_us = esp_timer_get_time() + (20ULL * 1000000ULL);

            while (esp_timer_get_time() < countdown_end_us) {
                if (gpio_get_level(PIR_PIN) == 1) {
                    screen_off_deadline_us = esp_timer_get_time() + SCREEN_TIMEOUT_US;
                }

                int remaining_sec = (int)((countdown_end_us - esp_timer_get_time() + 999999ULL) / 1000000ULL);
                if (remaining_sec < 0) remaining_sec = 0;

                camera_fb_t *fb = esp_camera_fb_get();
                if (fb) {
                    decode_and_render_preview(fb, remaining_sec, (remaining_sec <= 3) ? "CHUAN BI CHUP..." : "CAN MAT VAO KHUNG");
                    esp_camera_fb_return(fb);
                }

                vTaskDelay(pdMS_TO_TICKS(40)); // ~20 FPS
            }

            // =========================================================
            // BƯỚC 3: HẾT 20S -> BẮT ĐẦU CHỤP BẰNG CẢ 2 CAMERA
            // =========================================================
            ESP_LOGI(TAG, ">>> [CAPTURE] Đã hết 20s! Bắt đầu chụp bằng cả 2 camera... <<<");

            // Tạo Session ID
            char session_id[32];
            snprintf(session_id, sizeof(session_id), "SES_%lu", (unsigned long)(esp_timer_get_time() / 1000));
            strncpy(current_session_id, session_id, sizeof(current_session_id) - 1);
            current_session_id[sizeof(current_session_id) - 1] = '\0';

            // Gửi lệnh chụp qua ESP-NOW cho Slave
            slave_ack_received = false;
            slave_upload_done = false;

            ControlMessage msg = {0};
            msg.msg_type = MSG_CMD_CAPTURE;
            strncpy(msg.session_id, session_id, sizeof(msg.session_id) - 1);

            for (int attempt = 1; attempt <= 3; attempt++) {
                esp_now_send(broadcastAddress, (uint8_t *)&msg, sizeof(ControlMessage));
                for (int wait = 0; wait < 10; wait++) {
                    if (slave_ack_received) break;
                    vTaskDelay(pdMS_TO_TICKS(40));
                }
                if (slave_ack_received) break;
            }

            // Chụp frame chính thức từ camera Master OV5640
            camera_fb_t *fb_snap = esp_camera_fb_get();
            if (fb_snap == NULL) {
                ESP_LOGE(TAG, "Lỗi chụp ảnh Master OV5640!");
                continue;
            }

            // Hiển thị màn hình chờ xác thực AI
            st7789_show_ai_processing_screen(panel_handle, lcd_render_buf);

            // =========================================================
            // BƯỚC 4: GỬI ẢNH LÊN SERVER (AI NHẬN DIỆN CẢ 2 ẢNH)
            // =========================================================
            char url_with_session[256];
            snprintf(url_with_session, sizeof(url_with_session), "%s?session_id=%s", SERVER_URL, session_id);
            memset(http_response_buffer, 0, sizeof(http_response_buffer));

            esp_http_client_config_t http_cfg = {
                .url = url_with_session,
                .method = HTTP_METHOD_POST,
                .event_handler = http_event_handler,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = 30000,
                .skip_cert_common_name_check = true,
                .buffer_size_tx = 4096,
            };

            esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
            if (client != NULL) {
                esp_http_client_set_header(client, "Content-Type", "image/jpeg");
                esp_http_client_set_post_field(client, (const char *)fb_snap->buf, fb_snap->len);

                esp_err_t err = esp_http_client_perform(client);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "Server Response: %s", http_response_buffer);

                    bool is_unlock = (strstr(http_response_buffer, "\"status\":\"UNLOCK\"") != NULL ||
                                      strstr(http_response_buffer, "UNLOCK") != NULL);

                    char user_name[64] = "Thanh vien";
                    char *name_ptr = strstr(http_response_buffer, "\"user_name\":\"");
                    if (name_ptr) {
                        name_ptr += 13;
                        char *quote_end = strchr(name_ptr, '\"');
                        if (quote_end) {
                            int nlen = (int)(quote_end - name_ptr);
                            if (nlen > 0 && nlen < sizeof(user_name)) {
                                strncpy(user_name, name_ptr, nlen);
                                user_name[nlen] = '\0';
                            }
                        }
                    }

                    float confidence = 0.0f;
                    char *conf_ptr = strstr(http_response_buffer, "\"confidence\":");
                    if (conf_ptr) {
                        sscanf(conf_ptr + 13, "%f", &confidence);
                    }

                    if (is_unlock) {
                        ESP_LOGI(TAG, "=================================================");
                        ESP_LOGI(TAG, ">>> [XÁC THỰC THÀNH CÔNG] - %s (%.1f%%) <<<", user_name, confidence * 100.0f);
                        ESP_LOGI(TAG, ">>> [RELAY]: BẬT RELAY (GPIO %d) -> ĐÃ MỞ CỬA! <<<", RELAY_PIN);
                        ESP_LOGI(TAG, "=================================================");

                        gpio_set_level(RELAY_PIN, 1);
                        st7789_show_result_screen(panel_handle, lcd_render_buf, true, user_name, confidence);

                        vTaskDelay(pdMS_TO_TICKS(3000));

                        gpio_set_level(RELAY_PIN, 0);
                        ESP_LOGI(TAG, ">>> [RELAY]: TẮT RELAY -> ĐÃ ĐÓNG/KHÓA CỬA LẠI! <<<");
                    } else {
                        ESP_LOGW(TAG, "=================================================");
                        ESP_LOGW(TAG, ">>> [TỪ CHỐI TRUY CẬP] - CẢNH BÁO: NGƯỜI LẠ! <<<");
                        ESP_LOGW(TAG, ">>> [RELAY]: GIỮ NGUYÊN KHÓA CỬA! <<<");
                        ESP_LOGW(TAG, "=================================================");

                        st7789_show_result_screen(panel_handle, lcd_render_buf, false, "Nguoi la", confidence);
                        vTaskDelay(pdMS_TO_TICKS(3000));
                    }
                } else {
                    ESP_LOGE(TAG, "Lỗi kết nối Server: %s", esp_err_to_name(err));
                    st7789_fill_color(panel_handle, COLOR_RED);
                    vTaskDelay(pdMS_TO_TICKS(2000));
                }

                esp_http_client_cleanup(client);
            }

            esp_camera_fb_return(fb_snap);

            // Chờ Slave upload nếu chưa xong
            if (slave_ack_received && !slave_upload_done) {
                for (int w = 0; w < 30; w++) {
                    if (slave_upload_done) break;
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }

            // Cooldown 1 giây
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // =========================================================
        // KIỂM TRA TIMEOUT 5 PHÚT NẾU PIR KHÔNG THẤY CHUYỂN ĐỘNG
        // =========================================================
        if (screen_is_on) {
            int64_t now_us = esp_timer_get_time();
            if (now_us >= screen_off_deadline_us) {
                ESP_LOGI(TAG, ">>> [PIR TIMEOUT] Quá 5 phút không có chuyển động -> Tắt màn hình TFT <<<");
                screen_is_on = false;
                st7789_set_backlight(false);
                esp_lcd_panel_disp_on_off(panel_handle, false);
                st7789_fill_color(panel_handle, COLOR_BLACK);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* =========================================================
 * APP MAIN
 * ========================================================= */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }

    // PIR & Relay
    init_hardware();

    // Display ST7789
    ESP_ERROR_CHECK(st7789_init(&panel_handle));
    st7789_fill_color(panel_handle, COLOR_BLACK);

    // Cấp phát Render Buffer cho Live Preview trong PSRAM
    lcd_render_buf = (uint16_t *)heap_caps_malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lcd_render_buf) {
        ESP_LOGW(TAG, "Không thể cấp phát PSRAM cho render buffer, dùng internal RAM");
        lcd_render_buf = (uint16_t *)malloc(LCD_H_RES * LCD_V_RES * sizeof(uint16_t));
    }

    // Camera OV5640
    ESP_ERROR_CHECK(init_camera_ov5640());

    // WiFi
    init_wifi();

    // ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    esp_now_peer_info_t peerInfo = {0};
    memcpy(peerInfo.peer_addr, broadcastAddress, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&peerInfo));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_esp_now_recv));
    // Khoi dong Heartbeat Task cho Master
    xTaskCreate(
       master_heartbeat_task,
       "master_heartbeat",
       4096,    
       NULL,
       3,
       NULL
    );

    // Access Control Task
    xTaskCreate(access_control_task, "access_task", 8192, NULL, 5, NULL);
}