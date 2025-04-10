/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* ESP32-S3 牙刷工具服务器 WebSocket 客户端
 *
 * 实现设备与服务器的通信协议，包括：
 * - 设备上线
 * - 文件列表同步
 * - 文件下载
 * - 心跳保活
 */

#include <stdio.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "protocol_examples_common.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include <cJSON.h>

#define HEARTBEAT_INTERVAL_SEC  30  // 心跳间隔（秒）
#define DEVICE_VERSION          "1.0.0"
#define RECONNECT_INTERVAL_SEC  5   // 重连间隔（秒）
#define MAX_RECONNECT_ATTEMPTS  10  // 最大重连尝试次数

static const char *TAG = "esp_websocket_client";

// 全局变量
static esp_websocket_client_handle_t client = NULL;
static TimerHandle_t heartbeat_timer = NULL;
static TimerHandle_t reconnect_timer = NULL;  // 重连定时器
static int reconnect_attempts = 0;  // 重连尝试次数
static char device_mac[18];  // MAC地址字符串 "XX:XX:XX:XX:XX:XX"
static char device_id[32];   // 设备ID

// 模拟的文件列表
typedef struct {
    char filename[32];
    int size;
    char md5[33];  // MD5是32个字符
    int64_t timestamp;
} file_info_t;

#define MAX_FILES 5
static file_info_t device_files[MAX_FILES];
static int file_count = 0;

// 函数声明
static void generate_mock_files(void);
static void send_online_message(void);
static void send_file_list(void);
static void send_heartbeat(void);
static void handle_download_notify(const char *message);
static esp_err_t download_file(const char *url, const char *filename, const char *expected_md5, int file_size);
static void send_download_complete(const char *filename, const char *md5);
static void heartbeat_timer_callback(TimerHandle_t xTimer);
static void reconnect_timer_callback(TimerHandle_t xTimer);  // 重连定时器回调
static void reset_connection_state(void);  // 复位连接状态
static void attempt_reconnect(void);  // 尝试重连

// 日志辅助函数
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "错误 %s: 0x%x", message, error_code);
    }
}

// WebSocket事件处理函数
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket连接成功");
        // 重置重连计数
        reconnect_attempts = 0;
        // 停止重连定时器（如果正在运行）
        if (reconnect_timer != NULL) {
            xTimerStop(reconnect_timer, portMAX_DELAY);
        }
        // 发送设备上线消息
        send_online_message();
        // 启动心跳定时器
        xTimerStart(heartbeat_timer, portMAX_DELAY);
        break;
        
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket断开连接");
        // 停止心跳定时器
        xTimerStop(heartbeat_timer, portMAX_DELAY);
        log_error_if_nonzero("HTTP状态码", data->error_handle.esp_ws_handshake_status_code);
        
        // 复位连接状态
        reset_connection_state();
        
        // 启动重连定时器
        if (reconnect_timer != NULL) {
            xTimerStart(reconnect_timer, portMAX_DELAY);
        }
        break;
        
    case WEBSOCKET_EVENT_DATA:
        // 检查数据是否为空
        if (data->data_len <= 0 || data->data_ptr == NULL) {
            ESP_LOGW(TAG, "收到空消息，跳过处理");
            break;
        }
        
        // 检查是否为WebSocket控制帧（ping, pong, close等）
        if (data->op_code == 0x9) { // ping帧
            ESP_LOGD(TAG, "收到PING帧");
            // ESP-IDF WebSocket客户端会自动回复PONG，无需手动处理
            break;
        } else if (data->op_code == 0xA) { // pong帧
            ESP_LOGD(TAG, "收到PONG帧");
            break;
        } else if (data->op_code == 0x8) { // close帧
            ESP_LOGW(TAG, "收到CLOSE帧");
            // close帧也会由客户端库自动处理
            break;
        }
        
        ESP_LOGI(TAG, "收到数据: %.*s", data->data_len, (char *)data->data_ptr);
        
        // 确保数据以null结尾以便安全解析
        char *json_data = malloc(data->data_len + 1);
        if (json_data == NULL) {
            ESP_LOGE(TAG, "内存分配失败");
            break;
        }
        
        memcpy(json_data, data->data_ptr, data->data_len);
        json_data[data->data_len] = '\0';
        
        // 解析JSON消息
        cJSON *root = cJSON_Parse(json_data);
        if (root) {
            cJSON *type = cJSON_GetObjectItem(root, "type");
            if (type && type->valuestring) {
                ESP_LOGI(TAG, "消息类型: %s", type->valuestring);
                
                if (strcmp(type->valuestring, "online_ack") == 0) {
                    ESP_LOGI(TAG, "设备上线确认");
                    // 设备上线确认后发送文件列表
                    send_file_list();
                    
                } else if (strcmp(type->valuestring, "file_list_ack") == 0) {
                    ESP_LOGI(TAG, "文件列表确认");
                    
                } else if (strcmp(type->valuestring, "download_notify") == 0) {
                    // 处理下载通知
                    handle_download_notify(json_data);
                    
                } else if (strcmp(type->valuestring, "download_complete_ack") == 0) {
                    ESP_LOGI(TAG, "下载完成确认");
                    
                } else if (strcmp(type->valuestring, "heartbeat_ack") == 0) {
                    ESP_LOGD(TAG, "心跳确认");
                }
            } else {
                ESP_LOGW(TAG, "消息缺少type字段或格式不正确");
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGE(TAG, "JSON解析失败: %s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "未知错误");
            ESP_LOGD(TAG, "原始数据: [%.*s]", data->data_len, (char *)data->data_ptr);
        }
        
        free(json_data);
        break;
        
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        log_error_if_nonzero("HTTP状态码", data->error_handle.esp_ws_handshake_status_code);
        break;
    }
}

// 初始化设备信息
static void init_device_info(void)
{
    // 获取MAC地址
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
    snprintf(device_mac, sizeof(device_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
             
    // 生成设备ID（使用MAC地址后六位）
    snprintf(device_id, sizeof(device_id), "esp32-s3-%02x%02x%02x", mac_addr[3], mac_addr[4], mac_addr[5]);
    
    ESP_LOGI(TAG, "设备MAC: %s", device_mac);
    ESP_LOGI(TAG, "设备ID: %s", device_id);
    
    // 生成模拟的文件列表
    generate_mock_files();
}

// 生成模拟的文件列表
static void generate_mock_files(void)
{
    // 随机生成1-5个文件
    file_count = 1 + esp_random() % MAX_FILES;
    
    for (int i = 0; i < file_count; i++) {
        snprintf(device_files[i].filename, sizeof(device_files[i].filename), "file%d.bin", i+1);
        device_files[i].size = 1024 + esp_random() % 10240; // 1KB到10KB
        device_files[i].timestamp = esp_timer_get_time() / 1000000; // 当前时间（秒）
        
        // 生成假的MD5（在实际应用中应该计算真实MD5）
        char temp[64];
        snprintf(temp, sizeof(temp), "%s-%d-%lld", device_files[i].filename, 
                 device_files[i].size, device_files[i].timestamp);
        
        // 简单模拟MD5计算
        uint8_t digest[16];
        for (int j = 0; j < 16; j++) {
            digest[j] = temp[j % strlen(temp)];
        }
        
        for (int j = 0; j < 16; j++) {
            snprintf(&device_files[i].md5[j*2], 3, "%02x", digest[j]);
        }
    }
    
    ESP_LOGI(TAG, "已生成%d个模拟文件", file_count);
}

// 发送设备上线消息
static void send_online_message(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "type", "online");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "version", DEVICE_VERSION);
    cJSON_AddStringToObject(data, "mac", device_mac);
    
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "发送上线消息: %s", json_str);
    
    esp_websocket_client_send_text(client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    cJSON_Delete(root);
}

// 发送文件列表
static void send_file_list(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    
    cJSON_AddStringToObject(root, "type", "file_list");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddItemToObject(data, "files", files);
    
    for (int i = 0; i < file_count; i++) {
        cJSON *file = cJSON_CreateObject();
        cJSON_AddStringToObject(file, "filename", device_files[i].filename);
        cJSON_AddNumberToObject(file, "size", device_files[i].size);
        cJSON_AddStringToObject(file, "md5", device_files[i].md5);
        cJSON_AddNumberToObject(file, "timestamp", device_files[i].timestamp);
        cJSON_AddItemToArray(files, file);
    }
    
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "发送文件列表: %s", json_str);
    
    esp_websocket_client_send_text(client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    cJSON_Delete(root);
}

// 发送心跳
static void send_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "type", "heartbeat");
    cJSON_AddNumberToObject(root, "timestamp", esp_timer_get_time() / 1000000); // 当前时间（秒）
    
    char *json_str = cJSON_Print(root);
    ESP_LOGD(TAG, "发送心跳: %s", json_str);
    
    esp_websocket_client_send_text(client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    cJSON_Delete(root);
}

// 心跳定时器回调
static void heartbeat_timer_callback(TimerHandle_t xTimer)
{
    send_heartbeat();
}

// 重连定时器回调
static void reconnect_timer_callback(TimerHandle_t xTimer)
{
    attempt_reconnect();
}

// 复位连接状态
static void reset_connection_state(void)
{
    // 复位所有与连接相关的状态
    // 可以在此添加任何需要在断开连接时重置的状态
    ESP_LOGI(TAG, "正在复位连接状态");
}

// 尝试重连
static void attempt_reconnect(void)
{
    if (reconnect_attempts < MAX_RECONNECT_ATTEMPTS) {
        reconnect_attempts++;
        ESP_LOGI(TAG, "尝试重连 (%d/%d)...", reconnect_attempts, MAX_RECONNECT_ATTEMPTS);
        
        // 如果客户端已停止，则重新启动
        if (client != NULL && !esp_websocket_client_is_connected(client)) {
            ESP_LOGI(TAG, "正在重新连接到WebSocket服务器...");
            esp_websocket_client_start(client);
        }
    } else {
        ESP_LOGW(TAG, "达到最大重连尝试次数 (%d)，停止重连", MAX_RECONNECT_ATTEMPTS);
        xTimerStop(reconnect_timer, portMAX_DELAY);
        // 可以在此添加其他失败处理逻辑，如重启设备等
    }
}

// 处理下载通知
static void handle_download_notify(const char *message)
{
    cJSON *root = cJSON_Parse(message);
    if (!root) {
        ESP_LOGE(TAG, "解析下载通知JSON失败");
        return;
    }
    
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        char *filename = NULL;
        char *url = NULL;
        char *md5 = NULL;
        int size = 0;
        
        cJSON *json_filename = cJSON_GetObjectItem(data, "filename");
        cJSON *json_url = cJSON_GetObjectItem(data, "url");
        cJSON *json_md5 = cJSON_GetObjectItem(data, "md5");
        cJSON *json_size = cJSON_GetObjectItem(data, "size");
        
        if (json_filename && json_filename->valuestring) filename = json_filename->valuestring;
        if (json_url && json_url->valuestring) url = json_url->valuestring;
        if (json_md5 && json_md5->valuestring) md5 = json_md5->valuestring;
        if (json_size) size = json_size->valueint;
        
        if (filename && url && md5 && size > 0) {
            ESP_LOGI(TAG, "收到下载通知: 文件=%s, URL=%s, MD5=%s, 大小=%d", 
                     filename, url, md5, size);
            
            // 发送下载确认
            cJSON *ack = cJSON_CreateObject();
            cJSON *ack_data = cJSON_CreateObject();
            
            cJSON_AddStringToObject(ack, "type", "download_ack");
            cJSON_AddStringToObject(ack, "status", "success");
            cJSON_AddStringToObject(ack, "message", "开始下载文件");
            cJSON_AddItemToObject(ack, "data", ack_data);
            cJSON_AddStringToObject(ack_data, "filename", filename);
            
            char *ack_str = cJSON_Print(ack);
            ESP_LOGI(TAG, "发送下载确认: %s", ack_str);
            
            esp_websocket_client_send_text(client, ack_str, strlen(ack_str), portMAX_DELAY);
            free(ack_str);
            cJSON_Delete(ack);
            
            // 开始下载文件
            esp_err_t ret = download_file(url, filename, md5, size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "文件下载失败");
            }
        } else {
            ESP_LOGE(TAG, "下载通知缺少必要字段");
        }
    }
    
    cJSON_Delete(root);
}

// HTTP事件处理函数
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static int file_size = 0;
    static char *file_buffer = NULL;
    static int received_size = 0;
    
    switch(evt->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (strcmp(evt->header_key, "Content-Length") == 0) {
                file_size = atoi(evt->header_value);
                ESP_LOGI(TAG, "文件大小: %d", file_size);
                
                // 分配内存用于存储文件
                if (file_buffer) {
                    free(file_buffer);
                }
                file_buffer = malloc(file_size);
                if (!file_buffer) {
                    ESP_LOGE(TAG, "内存分配失败");
                    return ESP_FAIL;
                }
                received_size = 0;
            }
            break;
            
        case HTTP_EVENT_ON_DATA:
            if (file_buffer) {
                if (received_size + evt->data_len <= file_size) {
                    memcpy(file_buffer + received_size, evt->data, evt->data_len);
                    received_size += evt->data_len;
                }
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            if (file_buffer && received_size == file_size) {
                ESP_LOGI(TAG, "文件下载完成，大小: %d", received_size);
                
                // 在HTTP_EVENT_ON_FINISH中，可以处理下载的文件
                // 但具体操作应该在download_file函数中完成
            }
            break;
            
        case HTTP_EVENT_DISCONNECTED:
            if (file_buffer) {
                free(file_buffer);
                file_buffer = NULL;
            }
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

// 下载文件
static esp_err_t download_file(const char *url, const char *filename, const char *expected_md5, int file_size)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
    };
    
    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (http_client == NULL) {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "开始下载文件: %s", url);
    
    esp_err_t err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP请求失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(http_client);
        return err;
    }
    
    int status_code = esp_http_client_get_status_code(http_client);
    ESP_LOGI(TAG, "HTTP状态码: %d", status_code);
    
    if (status_code == 200) {
        // 实际应用中应该将文件保存到存储，并计算真实MD5
        // 这里仅做示例，简单地比较MD5
        char calculated_md5[33] = "e10adc3949ba59abbe56e057f20f883e"; // 假设为计算所得MD5
        
        // 发送下载完成通知
        send_download_complete(filename, calculated_md5);
    }
    
    esp_http_client_cleanup(http_client);
    return ESP_OK;
}

// 发送下载完成通知
static void send_download_complete(const char *filename, const char *md5)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "type", "download_complete");
    cJSON_AddStringToObject(root, "status", "success");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "filename", filename);
    cJSON_AddStringToObject(data, "md5", md5);
    
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "发送下载完成通知: %s", json_str);
    
    esp_websocket_client_send_text(client, json_str, strlen(json_str), portMAX_DELAY);
    free(json_str);
    cJSON_Delete(root);
}

// 初始化并启动WebSocket客户端
static void websocket_app_start(void)
{
    char ws_url[128];
    
    // 初始化设备信息
    init_device_info();
    
    // 构建WebSocket URL，使用设备ID作为路径参数
    snprintf(ws_url, sizeof(ws_url), "%s%s", CONFIG_WEBSOCKET_URI, device_id);
    ESP_LOGI(TAG, "WebSocket URL: %s", ws_url);
    
    esp_websocket_client_config_t websocket_cfg = {
        .uri = ws_url,
    };
    
#if CONFIG_WS_OVER_TLS_MUTUAL_AUTH
    /* 配置客户端证书进行双向认证 */
    extern const char cacert_start[] asm("_binary_ca_cert_pem_start"); // CA证书
    extern const char cert_start[] asm("_binary_client_cert_pem_start"); // 客户端证书
    extern const char cert_end[]   asm("_binary_client_cert_pem_end");
    extern const char key_start[] asm("_binary_client_key_pem_start"); // 客户端私钥
    extern const char key_end[]   asm("_binary_client_key_pem_end");

    websocket_cfg.cert_pem = cacert_start;
    websocket_cfg.client_cert = cert_start;
    websocket_cfg.client_cert_len = cert_end - cert_start;
    websocket_cfg.client_key = key_start;
    websocket_cfg.client_key_len = key_end - key_start;
#elif CONFIG_WS_OVER_TLS_SERVER_AUTH
    // 使用证书包作为默认服务器证书源
    websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif

#if CONFIG_WS_OVER_TLS_SKIP_COMMON_NAME_CHECK
    websocket_cfg.skip_cert_common_name_check = true;
#endif
    
    // 初始化WebSocket客户端
    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    
    // 创建心跳定时器
    heartbeat_timer = xTimerCreate("Heartbeat", HEARTBEAT_INTERVAL_SEC * 1000 / portTICK_PERIOD_MS,
                                   pdTRUE, NULL, heartbeat_timer_callback);
    
    // 创建重连定时器
    reconnect_timer = xTimerCreate("Reconnect", RECONNECT_INTERVAL_SEC * 1000 / portTICK_PERIOD_MS,
                                   pdTRUE, NULL, reconnect_timer_callback);
    
    // 连接WebSocket服务器
    ESP_LOGI(TAG, "正在连接到 %s...", websocket_cfg.uri);
    esp_websocket_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] 启动...");
    ESP_LOGI(TAG, "[APP] 可用内存: %" PRIu32 " 字节", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF版本: %s", esp_get_idf_version());
    
    // 设置日志级别
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("websocket_client", ESP_LOG_DEBUG);
    esp_log_level_set("transport_ws", ESP_LOG_DEBUG);
    esp_log_level_set("trans_tcp", ESP_LOG_DEBUG);
    
    // 初始化ESP-IDF组件
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 配置WiFi连接
    ESP_ERROR_CHECK(example_connect());
    
    // 初始化并启动WebSocket客户端
    websocket_app_start();
}
