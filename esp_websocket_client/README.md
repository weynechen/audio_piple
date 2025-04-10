# ESP32-S3 音频文件服务器 WebSocket 客户端

这个应用程序实现了ESP32-S3设备与音频文件服务器之间的WebSocket通信。

## 功能特性

* 设备自动连接和断线重连机制
* 心跳保活
* 文件传输 (上传/下载)
* 文件列表同步
* 基于WebSocket的实时状态更新


## 硬件要求

本应用程序需要一块ESP32-S3开发板，唯一的必要接口是WiFi以连接到互联网或本地服务器。

## 配置项目

* 打开项目配置菜单 (`idf.py menuconfig`)
* 在"Example Connection Configuration"菜单下配置WiFi连接信息
* 在"Example Configuration"菜单下配置WebSocket端点URI，如果选择"WEBSOCKET_URI_FROM_STDIN"，则应用程序将从标准输入读取URI (用于测试)
* 禁用TLS

### WebSocket服务器URL格式

默认的WebSocket服务器URL格式为：`ws://your-server-ip:8001/ws/{client_id}`，其中`{client_id}`是基于设备MAC地址的唯一标识符。

### 服务器证书验证
禁用证书

## 构建和烧录

构建项目并将其烧录到开发板，然后运行监视工具查看串口输出：

```
idf.py -p PORT flash monitor
```

(使用`Ctrl-]`退出串口监视器)

有关配置和使用ESP-IDF构建项目的完整步骤，请参阅入门指南。

## 文件存储

该应用使用SPIFFS文件系统来存储下载的文件。文件系统会在首次启动时自动格式化。

## 与服务器的通信协议

设备与服务器之间使用JSON格式的消息进行通信：

1. **设备上线消息**：
   ```json
   {
     "type": "online",
     "device_id": "esp32_xxxxxxxxxxxx",
     "version": "1.0.0"
   }
   ```

2. **心跳消息**：
   ```json
   {
     "type": "heartbeat",
     "device_id": "esp32_xxxxxxxxxxxx",
     "timestamp": 1234567890
   }
   ```

3. **文件列表消息**：
   ```json
   {
     "type": "file_list",
     "device_id": "esp32_xxxxxxxxxxxx",
     "files": [
       {"name": "file1.txt", "size": 1024, "md5": "..."},
       {"name": "file2.txt", "size": 2048, "md5": "..."}
     ]
   }
   ```

4. **文件下载请求**：
   ```json
   {
     "type": "download",
     "url": "http://server/download/file.txt",
     "filename": "file.txt",
     "md5": "...",
     "size": 1024
   }
   ```

## 示例输出

```
I (482) esp_websocket_client: [APP] 启动...
I (2492) example_connect: WiFi Link Up
I (4472) tcpip_adapter: sta ip: 192.168.1.100, mask: 255.255.255.0, gw: 192.168.1.1
I (4472) example_connect: 已连接到WiFi
I (4472) example_connect: IPv4地址: 192.168.1.100
I (4482) esp_websocket_client: WebSocket URL: ws://192.168.1.10:8001/ws/esp32_aabbccddeeff
I (5012) esp_websocket_client: WebSocket连接成功
I (5492) esp_websocket_client: 发送上线消息
I (6052) esp_websocket_client: 收到数据: {"type":"heartbeat_ack"}
I (7492) esp_websocket_client: 发送心跳
I (8082) esp_websocket_client: 收到数据: {"type":"heartbeat_ack"}
```

## 文件分发服务器

本WebSocket客户端需要配合[文件分发服务器](../file_distribution_server/README.md)使用。服务器提供了以下功能：

- WebSocket长连接管理
- 文件上传和下载
- 设备在线状态监控
- 文件处理和分发

请确保您已经正确设置并运行了文件分发服务器。
