/*
 * SPDX-FileCopyrightText: 2022-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* ESP32-S3 音频文件服务器 WebSocket 客户端
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
#include "esp_spiffs.h"
#include <dirent.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include <cJSON.h>
#include "mbedtls/md5.h"

// 增加定时器服务任务栈大小
#define TIMER_SERVICE_TASK_STACK_SIZE 8192  // 之前是4096，增加到8192

#define HEARTBEAT_INTERVAL_SEC  5  // 心跳间隔（秒）
#define DEVICE_VERSION          "1.0.0"
#define RECONNECT_INTERVAL_SEC  5   // 重连间隔（秒）
#define MAX_RECONNECT_ATTEMPTS  10  // 最大重连尝试次数
#define PROGRESS_BAR_WIDTH      50  // 进度条宽度
#define MAX_FILE_SIZE           (1024*1024)  // 最大文件大小限制(1MB)
#define BUFFER_SIZE             4096   // 文件传输缓冲区大小
#define WS_TASK_STACK_SIZE      4096  // WebSocket处理任务栈大小
#define WS_TASK_PRIORITY        5     // WebSocket处理任务优先级
#define WS_QUEUE_SIZE           10    // WebSocket事件队列大小

static const char *TAG = "esp_websocket_client";

// 全局变量
static esp_websocket_client_handle_t client = NULL;
static TimerHandle_t heartbeat_timer = NULL;
static TimerHandle_t reconnect_timer = NULL;  // 重连定时器
static int reconnect_attempts = 0;  // 重连尝试次数
static char device_mac[18];  // MAC地址字符串 "XX:XX:XX:XX:XX:XX"
static char device_id[32];   // 设备ID

// WebSocket事件处理任务和队列
static QueueHandle_t ws_event_queue = NULL;
static TaskHandle_t ws_task_handle = NULL;

// WebSocket事件类型
typedef enum {
    WS_EVENT_CONNECTED,
    WS_EVENT_DISCONNECTED,
    WS_EVENT_DATA,
    WS_EVENT_ERROR
} ws_event_type_t;

// WebSocket事件消息结构
typedef struct {
    ws_event_type_t event_type;  // 事件类型
    union {
        struct {
            int status_code;  // 状态码（用于断开连接和错误事件）
        } conn_info;
        struct {
            char *data;       // 数据指针
            int data_len;     // 数据长度
            int op_code;      // WebSocket操作码
        } data_info;
    };
} ws_event_msg_t;

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
static void send_online_message(void);
static void send_file_list(void);
static esp_err_t download_file(const char *url, const char *filename, const char *expected_md5, int file_size);
static void send_download_complete(const char *filename, const char *md5);
static esp_err_t upload_file(const char *filename, const char *url);
static void handle_upload_request(const char *message);
static void send_upload_complete(const char *filename, const char *md5);
static void heartbeat_timer_callback(TimerHandle_t xTimer);
static void reconnect_timer_callback(TimerHandle_t xTimer);  // 重连定时器回调
static void reset_connection_state(void);  // 复位连接状态
static void attempt_reconnect(void);  // 尝试重连
static esp_err_t init_spiffs(void);
static void send_progress_notification(const char *type, const char *filename, int percent, int transferred, int total_size);
static void ws_event_task(void *pvParameter); // WebSocket事件处理任务
static void handle_ws_event(ws_event_msg_t *msg); // 处理WebSocket事件

// 日志辅助函数
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "错误 %s: 0x%x", message, error_code);
    }
}

// 简单地检查和提取消息类型，不使用cJSON
static const char* get_message_type(const char* json_str) {
    static char type_buffer[32];
    char* type_start = strstr(json_str, "\"type\":\"");
    if (type_start) {
        type_start += 8; // 跳过 "type":"
        char* type_end = strchr(type_start, '\"');
        if (type_end && (type_end - type_start < sizeof(type_buffer) - 1)) {
            int len = type_end - type_start;
            memcpy(type_buffer, type_start, len);
            type_buffer[len] = '\0';
            return type_buffer;
        }
    }
    return NULL;
}

// 修改WebSocket事件处理函数，只将事件放入队列
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket连接成功");
        // 直接处理连接事件
        reconnect_attempts = 0;
        if (reconnect_timer != NULL) {
            xTimerStop(reconnect_timer, portMAX_DELAY);
        }
        send_online_message();
        xTimerStart(heartbeat_timer, portMAX_DELAY);
        break;
        
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "WebSocket断开连接");
        // 直接处理断开连接事件
        xTimerStop(heartbeat_timer, portMAX_DELAY);
        log_error_if_nonzero("HTTP状态码", data->error_handle.esp_ws_handshake_status_code);
        reset_connection_state();
        if (reconnect_timer != NULL) {
            xTimerStart(reconnect_timer, portMAX_DELAY);
        }
        break;
        
    case WEBSOCKET_EVENT_DATA:
        // 检查数据是否为空
        if (data->data_len <= 0 || data->data_ptr == NULL) {
            ESP_LOGW(TAG, "收到空消息，跳过处理");
            return;
        }
        
        // 检查是否为WebSocket控制帧（ping, pong, close等）
        if (data->op_code == 0x9) { // ping帧
            ESP_LOGD(TAG, "收到PING帧");
            // ESP-IDF WebSocket客户端会自动回复PONG，无需手动处理
            return;
        } else if (data->op_code == 0xA) { // pong帧
            ESP_LOGD(TAG, "收到PONG帧");
            return;
        } else if (data->op_code == 0x8) { // close帧
            ESP_LOGW(TAG, "收到CLOSE帧");
            // 处理断开连接事件
            xTimerStop(heartbeat_timer, portMAX_DELAY);
            reset_connection_state();
            if (reconnect_timer != NULL) {
                xTimerStart(reconnect_timer, portMAX_DELAY);
            }
            return;
        }
        
        ESP_LOGI(TAG, "收到数据: %.*s", data->data_len, (char *)data->data_ptr);
        
        // 为数据分配内存并复制
        char *json_data = malloc(data->data_len + 1);
        if (json_data == NULL) {
            ESP_LOGE(TAG, "内存分配失败");
            return;
        }
        
        memcpy(json_data, data->data_ptr, data->data_len);
        json_data[data->data_len] = '\0';
        
        // 先检查是否为简单确认消息，直接处理
        const char* msg_type = get_message_type(json_data);
        if (msg_type) {
            if (strcmp(msg_type, "online_ack") == 0) {
                ESP_LOGI(TAG, "设备上线确认");
                send_file_list();
                free(json_data);
                return; // 直接处理完成，返回
            } else if (strcmp(msg_type, "file_list_ack") == 0) {
                ESP_LOGI(TAG, "文件列表确认");
                free(json_data);
                return; // 直接处理完成，返回
            } else if (strcmp(msg_type, "heartbeat_ack") == 0) {
                ESP_LOGD(TAG, "心跳确认");
                free(json_data);
                return; // 直接处理完成，返回
            }
        }
        
        // 其他复杂消息通过队列处理
        ws_event_msg_t msg;
        msg.event_type = WS_EVENT_DATA;
        msg.data_info.data = json_data;
        msg.data_info.data_len = data->data_len;
        msg.data_info.op_code = data->op_code;
        
        // 将消息发送到队列，使用非阻塞方式（最多等待10 tick）
        if (xQueueSend(ws_event_queue, &msg, 10 / portTICK_PERIOD_MS) != pdTRUE) {
            ESP_LOGW(TAG, "WebSocket事件队列已满，丢弃事件");
            free(json_data);
        }
        break;
        
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket错误");
        // 错误事件通过队列处理
        ws_event_msg_t error_msg;
        error_msg.event_type = WS_EVENT_ERROR;
        error_msg.conn_info.status_code = data->error_handle.esp_ws_handshake_status_code;
        
        if (xQueueSend(ws_event_queue, &error_msg, 10 / portTICK_PERIOD_MS) != pdTRUE) {
            ESP_LOGW(TAG, "WebSocket事件队列已满，丢弃错误事件");
        }
        break;
        
    default:
        // 未知事件类型，跳过处理
        break;
    }
}

// WebSocket事件处理任务
static void ws_event_task(void *pvParameter)
{
    ws_event_msg_t msg;
    
    ESP_LOGI(TAG, "WebSocket事件处理任务已启动");
    
    while (1) {
        // 从队列接收消息，无限等待
        if (xQueueReceive(ws_event_queue, &msg, portMAX_DELAY)) {
            // 处理WebSocket事件
            handle_ws_event(&msg);
            
            // 如果是数据事件，需要释放已分配的内存
            if (msg.event_type == WS_EVENT_DATA) {
                free(msg.data_info.data);
            }
        }
    }
}

// 处理WebSocket事件
static void handle_ws_event(ws_event_msg_t *msg)
{
    // 处理复杂的WebSocket事件
    // 注意：WS_EVENT_CONNECTED和WS_EVENT_DISCONNECTED事件已在回调中处理
    if (msg->event_type == WS_EVENT_DATA) {
        char *json_data = msg->data_info.data;
        
        // 检查消息类型
        const char* msg_type = get_message_type(json_data);
        if (msg_type) {
            ESP_LOGI(TAG, "任务处理消息类型: %s", msg_type);
            
            // 处理下载通知（复杂消息）
            if (strcmp(msg_type, "download_notify") == 0) {
                // 解析下载通知
                cJSON *root = cJSON_Parse(json_data);
                if (root) {
                    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
                    if (data_obj) {
                        char *filename = NULL;
                        char *url = NULL;
                        char *md5 = NULL;
                        int size = 0;
                        
                        cJSON *json_filename = cJSON_GetObjectItem(data_obj, "filename");
                        cJSON *json_url = cJSON_GetObjectItem(data_obj, "url");
                        cJSON *json_md5 = cJSON_GetObjectItem(data_obj, "md5");
                        cJSON *json_size = cJSON_GetObjectItem(data_obj, "size");
                        
                        if (json_filename && json_filename->valuestring) filename = json_filename->valuestring;
                        if (json_url && json_url->valuestring) url = json_url->valuestring;
                        if (json_md5 && json_md5->valuestring) md5 = json_md5->valuestring;
                        if (json_size) size = json_size->valueint;
                        
                        if (filename && url && md5 && size > 0) {
                            ESP_LOGI(TAG, "收到下载通知: 文件=%s, URL=%s, MD5=%s, 大小=%d", 
                                    filename, url, md5, size);
                            
                            // 发送下载确认
                            static char ack_buffer[256];
                            snprintf(ack_buffer, sizeof(ack_buffer),
                                    "{\"type\":\"download_ack\",\"status\":\"success\",\"message\":\"开始下载文件\",\"data\":{\"filename\":\"%s\"}}",
                                    filename);
                            
                            ESP_LOGI(TAG, "发送下载确认: %s", ack_buffer);
                            esp_websocket_client_send_text(client, ack_buffer, strlen(ack_buffer), portMAX_DELAY);
                            
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
            } 
            // 简单确认消息已在回调中处理，不需要在任务中再处理
        } else {
            // 处理其他复杂消息类型，尝试用cJSON解析
            cJSON *root = cJSON_Parse(json_data);
            if (root) {
                cJSON *type = cJSON_GetObjectItem(root, "type");
                if (type && type->valuestring) {
                    ESP_LOGI(TAG, "消息类型: %s", type->valuestring);
                    
                    if (strcmp(type->valuestring, "upload_request") == 0) {
                        // 处理上传请求
                        handle_upload_request(json_data);
                    } else if (strcmp(type->valuestring, "download_complete_ack") == 0) {
                        ESP_LOGI(TAG, "下载完成确认");
                    } else if (strcmp(type->valuestring, "upload_complete_ack") == 0) {
                        ESP_LOGI(TAG, "上传完成确认");
                    }
                } else {
                    ESP_LOGW(TAG, "消息缺少type字段或格式不正确");
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "JSON解析失败: %s", cJSON_GetErrorPtr() ? cJSON_GetErrorPtr() : "未知错误");
            }
        }
    } else if (msg->event_type == WS_EVENT_ERROR) {
        // 错误事件处理
        log_error_if_nonzero("HTTP状态码", msg->conn_info.status_code);
    }
    // WS_EVENT_CONNECTED和WS_EVENT_DISCONNECTED不需要处理，因为已在回调中处理
}

// 初始化设备信息
static void init_device_info(void)
{
    // 获取MAC地址
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_WIFI_STA);
    snprintf(device_mac, sizeof(device_mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
             
    // 获取随机数(0-999)
    uint32_t random_num = esp_random() % 1000;
    
    // 生成设备ID（使用MAC地址后六位和随机数字）
    snprintf(device_id, sizeof(device_id), "esp32-s3-%02x%02x%02x_%lu", 
             mac_addr[3], mac_addr[4], mac_addr[5], (unsigned long)random_num);
    
    ESP_LOGI(TAG, "设备MAC: %s", device_mac);
    ESP_LOGI(TAG, "设备ID: %s", device_id);
}


// 发送设备上线消息
static void send_online_message(void)
{
    // 使用静态缓冲区减少栈使用
    static char json_buffer[256];
    
    // 直接格式化JSON字符串，避免使用cJSON库的动态内存分配
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"type\":\"online\",\"data\":{\"version\":\"%s\",\"mac\":\"%s\"}}",
             DEVICE_VERSION, device_mac);
    
    ESP_LOGI(TAG, "发送上线消息: %s", json_buffer);
    
    esp_websocket_client_send_text(client, json_buffer, strlen(json_buffer), portMAX_DELAY);
}

// 发送文件列表
static void send_file_list(void)
{
    // 使用静态缓冲区减少栈使用
    static char json_buffer[512];
    static char files_buffer[384]; // 用于存储文件数组部分
    
    // 先构建文件数组部分
    files_buffer[0] = '\0';
    for (int i = 0; i < file_count; i++) {
        char file_json[128];
        snprintf(file_json, sizeof(file_json),
                "%s{\"filename\":\"%s\",\"size\":%d,\"md5\":\"%s\",\"timestamp\":%lld}",
                (i > 0 ? "," : ""), 
                device_files[i].filename, 
                device_files[i].size,
                device_files[i].md5,
                device_files[i].timestamp);
        
        // 确保不会缓冲区溢出
        if (strlen(files_buffer) + strlen(file_json) < sizeof(files_buffer) - 1) {
            strcat(files_buffer, file_json);
        } else {
            ESP_LOGW(TAG, "文件列表过长，已截断");
            break;
        }
    }
    
    // 构建完整JSON
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"type\":\"file_list\",\"data\":{\"files\":[%s]}}",
             files_buffer);
    
    ESP_LOGI(TAG, "发送文件列表: %s", json_buffer);
    
    esp_websocket_client_send_text(client, json_buffer, strlen(json_buffer), portMAX_DELAY);
}

// 心跳定时器回调
static void heartbeat_timer_callback(TimerHandle_t xTimer)
{
    // 简化心跳发送过程，减少栈使用
    static char heartbeat_msg[64];
    int64_t timestamp = esp_timer_get_time() / 1000000; // 当前时间（秒）
    
    snprintf(heartbeat_msg, sizeof(heartbeat_msg), 
             "{\"type\":\"heartbeat\",\"timestamp\":%lld}", timestamp);
    
    ESP_LOGD(TAG, "发送心跳");
    esp_websocket_client_send_text(client, heartbeat_msg, strlen(heartbeat_msg), 0);
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

// 初始化SPIFFS
static esp_err_t init_spiffs(void)
{
    ESP_LOGI(TAG, "正在初始化SPIFFS");
    
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",  // 使用"spiffs"作为分区标签
        .max_files = 5,
        .format_if_mount_failed = true
    };
    
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "无法挂载SPIFFS分区，或格式化失败");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "未找到SPIFFS分区，请检查分区表");
        } else {
            ESP_LOGE(TAG, "初始化SPIFFS失败: %s", esp_err_to_name(ret));
        }
        return ret;
    }
    
    size_t total = 0, used = 0;
    ret = esp_spiffs_info("spiffs", &total, &used);  // 指定分区标签
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "获取SPIFFS信息失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "SPIFFS已挂载，总大小: %d字节, 已使用: %d字节, 空闲: %d字节", 
             total, used, total - used);
    
    // 列出SPIFFS根目录内容
    ESP_LOGI(TAG, "列出SPIFFS目录内容:");
    DIR *dir = opendir("/spiffs");
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            ESP_LOGI(TAG, "  %s", entry->d_name);
        }
        closedir(dir);
    } else {
        ESP_LOGE(TAG, "无法打开SPIFFS根目录");
    }
    
    return ESP_OK;
}

#pragma GCC diagnostic ignored "-Wformat-truncation"


// 在文件中添加一个简化的进度通知发送函数，减少栈使用
static void send_progress_notification(const char *type, const char *filename, int percent, int transferred, int total_size) 
{
    // 使用静态缓冲区减少栈使用
    static char json_buffer[256];
    
    // 直接格式化JSON字符串，避免使用cJSON库的动态内存分配
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"type\":\"%s\",\"data\":{\"filename\":\"%s\",\"percent\":%d,\"transferred\":%d,\"total_size\":%d}}",
             type, filename, percent, transferred, total_size);
    
    ESP_LOGD(TAG, "发送%s进度通知: %d%%", type, percent);
    
    // 使用0超时时间，避免在发送队列满时阻塞
    esp_websocket_client_send_text(client, json_buffer, strlen(json_buffer), 0);
}

// 下载文件
static esp_err_t download_file(const char *url, const char *filename, const char *expected_md5, int file_size)
{
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        // 添加超时设置，避免下载卡住
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (http_client == NULL) {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "开始下载文件: %s", url);
    
    // 检查SPIFFS可用空间
    size_t total = 0, used = 0;
    esp_err_t ret = esp_spiffs_info("spiffs", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "无法获取SPIFFS信息: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(http_client);
        return ret;
    }
    
    size_t free_space = total - used;
    ESP_LOGI(TAG, "SPIFFS可用空间: %d字节, 需要空间: %d字节", free_space, file_size);
    
    if (free_space < file_size) {
        ESP_LOGE(TAG, "SPIFFS空间不足，可用: %d字节, 需要: %d字节", free_space, file_size);
        
        // 尝试删除一些文件以释放空间
        ESP_LOGI(TAG, "尝试删除旧文件释放空间...");
        DIR *dir = opendir("/spiffs");
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                char file_path[64]; // 使用更小的缓冲区
                if (strlen(entry->d_name) < 30) { // 限制更短的文件名
                    snprintf(file_path, sizeof(file_path), "/spiffs/%s", entry->d_name);
                    
                    ESP_LOGI(TAG, "删除文件: %s", file_path);
                    unlink(file_path);
                    
                    // 重新检查空间
                    esp_spiffs_info("spiffs", &total, &used);
                    free_space = total - used;
                    if (free_space >= file_size) {
                        ESP_LOGI(TAG, "已释放足够空间: %d字节", free_space);
                        break;
                    }
                } else {
                    ESP_LOGW(TAG, "文件名过长，跳过: %.10s...", entry->d_name);
                }
            }
            closedir(dir);
        }
        
        // 再次检查空间
        if (free_space < file_size) {
            ESP_LOGE(TAG, "释放空间后仍然不足，无法下载文件");
            esp_http_client_cleanup(http_client);
            return ESP_ERR_NO_MEM;
        }
    }
    
    // 从源文件名中提取扩展名
    const char *ext = strrchr(filename, '.');
    if (!ext) ext = ""; // 如果没有扩展名
    
    // 生成短文件名：使用MD5的前8位 + 扩展名
    char short_filename[32];
    if (strlen(ext) > 0) {
        snprintf(short_filename, sizeof(short_filename), "f_%8.8s%s", expected_md5, ext);
    } else {
        snprintf(short_filename, sizeof(short_filename), "f_%8.8s", expected_md5);
    }
    
    ESP_LOGI(TAG, "使用短文件名: %s (原名: %s)", short_filename, filename);
    
    char file_path[64]; // 减小缓冲区大小
    snprintf(file_path, sizeof(file_path), "/spiffs/%s", short_filename);
    
    FILE *f = fopen(file_path, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "无法创建文件: %s, 错误: %s", file_path, strerror(errno));
        esp_http_client_cleanup(http_client);
        return ESP_FAIL;
    }
    
    // 开始HTTP请求
    esp_err_t err = esp_http_client_open(http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP请求失败: %s", esp_err_to_name(err));
        fclose(f);
        esp_http_client_cleanup(http_client);
        return err;
    }
    
    int content_length = esp_http_client_fetch_headers(http_client);
    ESP_LOGI(TAG, "文件大小: %d字节", content_length);
    
    // 文件大小检查
    if (content_length <= 0 || content_length > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "文件大小无效或过大: %d", content_length);
        fclose(f);
        esp_http_client_cleanup(http_client);
        return ESP_ERR_INVALID_SIZE;
    }
    
    // 计算MD5散列
    mbedtls_md5_context md5_ctx;
    mbedtls_md5_init(&md5_ctx);
    mbedtls_md5_starts(&md5_ctx);
    
    // 设置适当的缓冲区大小，降低内存使用
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        fclose(f);
        esp_http_client_cleanup(http_client);
        mbedtls_md5_free(&md5_ctx);
        return ESP_ERR_NO_MEM;
    }
    
    // 下载文件内容
    int total_read = 0;
    int read_len;
    
    int last_percent = 0;
    int last_update_time = 0;
    int64_t current_time;
    
    while (1) {
        read_len = esp_http_client_read(http_client, buffer, BUFFER_SIZE);
        if (read_len <= 0) {
            break;
        }
        
        // 写入文件
        size_t written = fwrite(buffer, 1, read_len, f);
        if (written != read_len) {
            ESP_LOGE(TAG, "文件写入错误: %d != %d", written, read_len);
            free(buffer);
            fclose(f);
            esp_http_client_cleanup(http_client);
            mbedtls_md5_free(&md5_ctx);
            return ESP_FAIL;
        }
        
        // 更新MD5散列
        mbedtls_md5_update(&md5_ctx, (const unsigned char *)buffer, read_len);
        
        total_read += read_len;
        // 计算下载百分比
        int percent = (total_read * 100) / content_length;
           
        // 发送进度通知到服务器
        // 修改进度更新逻辑，减少更新频率，避免栈溢出
        current_time = esp_timer_get_time() / 1000000; // 当前时间（秒）
        
        // 只在百分比变化大于10%或间隔至少3秒时才更新进度
        if (((percent - last_percent >= 10) || 
            (current_time - last_update_time >= 3)) && 
            percent != last_percent) {
            
            // 使用简化的进度通知函数
            send_progress_notification("download_progress", filename, percent, total_read, content_length);
            
            last_percent = percent;
            last_update_time = current_time;
        }
    }
    
    
    
    free(buffer);
    fclose(f);
    
    // 完成MD5计算
    unsigned char md5_result[16];
    char calculated_md5[33];
    mbedtls_md5_finish(&md5_ctx, md5_result);
    mbedtls_md5_free(&md5_ctx);
    
    // 转换为十六进制字符串
    for (int i = 0; i < 16; i++) {
        sprintf(calculated_md5 + i * 2, "%02x", md5_result[i]);
    }
    calculated_md5[32] = '\0';
    
    int status_code = esp_http_client_get_status_code(http_client);
    ESP_LOGI(TAG, "HTTP状态码: %d", status_code);
    esp_http_client_cleanup(http_client);
    
    if (status_code == 200) {
        ESP_LOGI(TAG, "文件下载完成，总大小: %d字节", total_read);
        ESP_LOGI(TAG, "计算的MD5: %s", calculated_md5);
        ESP_LOGI(TAG, "预期的MD5: %s", expected_md5);
        
        // 验证MD5
        if (strcmp(calculated_md5, expected_md5) == 0) {
            ESP_LOGI(TAG, "MD5校验成功");
        } else {
            ESP_LOGW(TAG, "MD5校验失败，可能文件已损坏");
        }
        
        // 发送下载完成通知，传递短文件名和原始文件名
        send_download_complete(short_filename, calculated_md5);
        
        // 保存文件到文件列表中
        if (file_count < MAX_FILES) {
            strncpy(device_files[file_count].filename, short_filename, sizeof(device_files[file_count].filename) - 1);
            device_files[file_count].size = total_read;
            strncpy(device_files[file_count].md5, calculated_md5, sizeof(device_files[file_count].md5) - 1);
            device_files[file_count].timestamp = esp_timer_get_time() / 1000000; // 当前时间（秒）
            file_count++;
            
            // 更新服务器上的文件列表
            send_file_list();
        }
        
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "HTTP下载失败，状态码: %d", status_code);
        return ESP_FAIL;
    }
}

// 发送下载完成通知
static void send_download_complete(const char *filename, const char *md5)
{
    // 使用静态缓冲区减少栈使用
    static char json_buffer[256];
    
    // 直接格式化JSON字符串
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"type\":\"download_complete\",\"status\":\"success\",\"data\":{\"filename\":\"%s\",\"md5\":\"%s\"}}",
             filename, md5);
    
    ESP_LOGI(TAG, "发送下载完成通知: %s", json_buffer);
    
    esp_websocket_client_send_text(client, json_buffer, strlen(json_buffer), portMAX_DELAY);
}

// 处理上传请求
static void handle_upload_request(const char *message)
{
    cJSON *root = cJSON_Parse(message);
    if (!root) {
        ESP_LOGE(TAG, "解析上传请求JSON失败");
        return;
    }
    
    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (data) {
        char *filename = NULL;
        char *url = NULL;
        
        cJSON *json_filename = cJSON_GetObjectItem(data, "filename");
        cJSON *json_url = cJSON_GetObjectItem(data, "url");
        
        if (json_filename && json_filename->valuestring) filename = json_filename->valuestring;
        if (json_url && json_url->valuestring) url = json_url->valuestring;
        
        if (filename && url) {
            ESP_LOGI(TAG, "收到上传请求: 文件=%s, URL=%s", filename, url);
            
            // 发送上传确认 - 使用静态缓冲区
            static char ack_buffer[256];
            snprintf(ack_buffer, sizeof(ack_buffer),
                    "{\"type\":\"upload_ack\",\"status\":\"success\",\"message\":\"开始上传文件\",\"data\":{\"filename\":\"%s\"}}",
                    filename);
            
            ESP_LOGI(TAG, "发送上传确认: %s", ack_buffer);
            esp_websocket_client_send_text(client, ack_buffer, strlen(ack_buffer), portMAX_DELAY);
            
            // 开始上传文件
            esp_err_t ret = upload_file(filename, url);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "文件上传失败");
            }
        } else {
            ESP_LOGE(TAG, "上传请求缺少必要字段");
        }
    }
    
    cJSON_Delete(root);
}

// 上传文件
static esp_err_t upload_file(const char *filename, const char *url)
{
    char file_path[64];
    snprintf(file_path, sizeof(file_path), "/spiffs/%s", filename);
    
    // 检查文件是否存在
    FILE *f = fopen(file_path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "无法打开文件: %s, 错误: %s", file_path, strerror(errno));
        return ESP_FAIL;
    }
    
    // 获取文件大小
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    // 文件大小检查
    if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "文件大小无效或过大: %ld", file_size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    
    ESP_LOGI(TAG, "开始上传文件: %s, 大小: %ld字节", filename, file_size);
    
    // 配置HTTP客户端进行POST请求
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        // 添加超时设置，避免上传卡住
        .timeout_ms = 30000,
    };
    
    esp_http_client_handle_t http_client = esp_http_client_init(&config);
    if (http_client == NULL) {
        ESP_LOGE(TAG, "HTTP客户端初始化失败");
        fclose(f);
        return ESP_FAIL;
    }
    
    // 设置HTTP头
    esp_http_client_set_header(http_client, "Content-Type", "application/octet-stream");
    char content_length_str[32];
    snprintf(content_length_str, sizeof(content_length_str), "%ld", file_size);
    esp_http_client_set_header(http_client, "Content-Length", content_length_str);
    esp_http_client_set_header(http_client, "X-Filename", filename);
    
    // 开始HTTP请求
    esp_err_t err = esp_http_client_open(http_client, file_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP请求失败: %s", esp_err_to_name(err));
        fclose(f);
        esp_http_client_cleanup(http_client);
        return err;
    }
    
    // 计算MD5散列
    mbedtls_md5_context md5_ctx;
    mbedtls_md5_init(&md5_ctx);
    mbedtls_md5_starts(&md5_ctx);
    
    // 设置适当的缓冲区大小，降低内存使用
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) {
        ESP_LOGE(TAG, "内存分配失败");
        fclose(f);
        esp_http_client_cleanup(http_client);
        mbedtls_md5_free(&md5_ctx);
        return ESP_ERR_NO_MEM;
    }
    
    // 上传文件内容
    int total_write = 0;
    size_t read_len;
    
    int last_percent = 0;
    int last_update_time = 0;
    int64_t current_time;
    
    while ((read_len = fread(buffer, 1, BUFFER_SIZE, f)) > 0) {
        int write_len = esp_http_client_write(http_client, buffer, read_len);
        if (write_len < 0) {
            ESP_LOGE(TAG, "HTTP写入错误");
            free(buffer);
            fclose(f);
            esp_http_client_cleanup(http_client);
            mbedtls_md5_free(&md5_ctx);
            return ESP_FAIL;
        }
        
        // 更新MD5散列
        mbedtls_md5_update(&md5_ctx, (const unsigned char *)buffer, read_len);
        
        total_write += write_len;
        int percent = (total_write * 100) / file_size;
    
        
        // 修改进度更新逻辑，减少更新频率，避免栈溢出
        current_time = esp_timer_get_time() / 1000000; // 当前时间（秒）
        
        // 只在百分比变化大于10%或间隔至少3秒时才更新进度
        if (((percent - last_percent >= 10) || 
            (current_time - last_update_time >= 3)) && 
            percent != last_percent) {
            
            // 使用简化的进度通知函数
            send_progress_notification("upload_progress", filename, percent, total_write, file_size);
            
            last_update_time = current_time;
            last_percent = percent;
        }
    }
    
    // 确保最终进度为100%
    if (last_percent != 100 && total_write == file_size) {
        // 发送100%进度通知
        send_progress_notification("upload_progress", filename, 100, total_write, file_size);
    }
    

    // 完成MD5计算
    unsigned char md5_result[16];
    char calculated_md5[33];
    mbedtls_md5_finish(&md5_ctx, md5_result);
    mbedtls_md5_free(&md5_ctx);
    
    // 转换为十六进制字符串
    for (int i = 0; i < 16; i++) {
        sprintf(calculated_md5 + i * 2, "%02x", md5_result[i]);
    }
    calculated_md5[32] = '\0';
    
    // 获取服务器响应
    int data_read = esp_http_client_read_response(http_client, buffer, sizeof(buffer) - 1);
    if (data_read >= 0) {
        buffer[data_read] = 0;
        ESP_LOGI(TAG, "服务器响应: %s", buffer);
    }
    
    int status_code = esp_http_client_get_status_code(http_client);
    ESP_LOGI(TAG, "HTTP状态码: %d", status_code);
    esp_http_client_cleanup(http_client);
    
    free(buffer);
    fclose(f);
    
    if (status_code == 200 || status_code == 201) {
        ESP_LOGI(TAG, "文件上传成功，总大小: %d字节", total_write);
        ESP_LOGI(TAG, "文件MD5: %s", calculated_md5);
        
        // 发送上传完成通知
        send_upload_complete(filename, calculated_md5);
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "文件上传失败，状态码: %d", status_code);
        return ESP_FAIL;
    }
}

// 发送上传完成通知
static void send_upload_complete(const char *filename, const char *md5)
{
    // 使用静态缓冲区减少栈使用
    static char json_buffer[256];
    
    // 直接格式化JSON字符串
    snprintf(json_buffer, sizeof(json_buffer),
             "{\"type\":\"upload_complete\",\"status\":\"success\",\"data\":{\"filename\":\"%s\",\"md5\":\"%s\"}}",
             filename, md5);
    
    ESP_LOGI(TAG, "发送上传完成通知: %s", json_buffer);
    
    esp_websocket_client_send_text(client, json_buffer, strlen(json_buffer), portMAX_DELAY);
}

// 初始化并启动WebSocket客户端
static void websocket_app_start(void)
{
    char ws_url[128];
    
    // 初始化设备信息
    init_device_info();
    
    // 创建WebSocket事件队列
    ws_event_queue = xQueueCreate(WS_QUEUE_SIZE, sizeof(ws_event_msg_t));
    if (ws_event_queue == NULL) {
        ESP_LOGE(TAG, "创建WebSocket事件队列失败");
        return;
    }
    
    // 创建WebSocket事件处理任务
    BaseType_t task_created = xTaskCreate(
        ws_event_task,                 // 任务函数
        "ws_event_task",               // 任务名称
        WS_TASK_STACK_SIZE,            // 任务栈大小
        NULL,                          // 任务参数
        WS_TASK_PRIORITY,              // 任务优先级
        &ws_task_handle                // 任务句柄
    );
    
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "创建WebSocket事件处理任务失败");
        vQueueDelete(ws_event_queue);
        return;
    }
    
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

// 应用程序入口
void app_main(void)
{
    ESP_LOGI(TAG, "[APP] 启动...");
    ESP_LOGI(TAG, "[APP] 可用内存: %" PRIu32 " 字节", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF版本: %s", esp_get_idf_version());
    
    // 设置日志级别 - 设置更详细的日志级别以便调试
    // 默认情况下使用INFO级别
    esp_log_level_set("*", ESP_LOG_INFO);
    // 为WebSocket客户端组件设置DEBUG级别
    esp_log_level_set("esp_websocket_client", ESP_LOG_DEBUG);
    esp_log_level_set("transport_ws", ESP_LOG_DEBUG);
    esp_log_level_set("trans_tcp", ESP_LOG_DEBUG);
    
    // 设置FreeRTOS定时器服务任务栈大小
    #if CONFIG_FREERTOS_USE_TIMERS
    // 使用默认配置时，需在menuconfig中修改CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH值
    ESP_LOGI(TAG, "定时器服务任务栈大小建议设置: %d字节", TIMER_SERVICE_TASK_STACK_SIZE);
    #endif
    
    // 初始化ESP-IDF组件
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    // 配置WiFi连接
    ESP_ERROR_CHECK(example_connect());
    
    // 初始化SPIFFS
    ESP_ERROR_CHECK(init_spiffs());
    
    // 初始化并启动WebSocket客户端
    websocket_app_start();
}
