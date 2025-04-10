# 音频文件处理服务器系统

本项目是一个完整的文件分发系统，用于音频文件的管理与更新。系统包含两个主要组件：

1. **文件分发服务器**：基于FastAPI和Gradio构建的Web服务器
2. **ESP32-S3客户端**：运行在ESP32-S3设备上的WebSocket客户端

## 系统功能

- 设备在线状态监控与管理
- 文件上传、处理与分发
- 自动文件下载与安装
- 实时状态更新与通知
- 断线重连与心跳保活
- 音频文件预览
（可扩展在后台做音频文件处理，然后再进行上传）

## 系统架构

```
┌─────────────────┐          ┌──────────────────┐
│                 │ WebSocket│                  │
│  文件分发服务器  ├──────────┤  ESP32-S3 客户端  │
│                 │          │                  │
└────────┬────────┘          └──────────────────┘
         │
         │ HTTP
         │
┌────────▼────────┐
│                 │
│    PC 客户端     │
│                 │
└─────────────────┘
```

## 组件文档

- [文件分发服务器](file_distribution_server/README.md) - 服务器部分的详细文档
- [ESP32-S3客户端](esp_websocket_client/README.md) - ESP32-S3客户端的详细文档

## 快速开始

### 启动服务器

1. 进入文件分发服务器目录:
```bash
cd file_distribution_server
```

2. 安装依赖并启动服务器:
```bash
uv sync
./run.sh
```

### 编译并烧录ESP32-S3客户端

1. 进入ESP32-S3客户端目录:
```bash
cd esp_websocket_client
```

2. 配置WiFi和服务器地址:
```bash
idf.py menuconfig
```

3. 编译并烧录:
```bash
idf.py -p PORT flash monitor
```

## 许可证
MIT
请参阅 [LICENSE](LICENSE) 文件。 