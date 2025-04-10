# 文件分发服务器

本项目是一个基于FastAPI和Gradio构建的文件处理和分发服务器。它允许PC客户端上传文件到服务器进行处理，处理后的文件可以被ESP32S3设备下载。

## 系统架构

- **服务器**：基于FastAPI的Web服务，包含文件上传、处理和下载功能
- **前端界面**：使用Gradio构建的简易操作界面
- **通信机制**：WebSocket用于与设备保持长连接

## 功能特性

- PC端文件上传
- 文件处理（可自定义处理逻辑）
- ESP32S3设备在线状态管理
- 处理后文件的自动通知和下载

## 安装
本项目使用uv管理开发环境
### 使用 uv 

[uv](https://github.com/astral-sh/uv) 是一个快速的 Python 包安装器和解析器。

1. 克隆代码仓库：

```bash
git clone https://github.com/your-username/file_distribution_server.git
cd file_distribution_server
```

2. 使用uv创建虚拟环境并安装项目：

```bash
uv sync
```

## 运行

```bash
uv run uvicorn main:app --reload --port 8001
```

服务器将在 `http://localhost:80001` 启动。本机可以访问。
如果需要给局域网内其他机器访问，可以使用脚本启动
```
chmod +x run.sh
./run.sh
```

## API接口

- **WebSocket连接**：`ws://server-ip:8000/ws/{client_id}`
- **文件上传**：`POST /upload/{client_id}`
- **文件下载**：`GET /download/{filename}`
- **获取在线设备**：`GET /clients`

## 前端界面

访问 `http://server-ip:8000` 打开Gradio界面进行文件上传和设备管理。

## 项目结构

```
file_distribution_server/
├── main.py              # 主应用入口
├── ws_manager.py        # WebSocket连接管理
├── file_processor.py    # 文件处理逻辑
├── static/              # 文件存储目录
├── pyproject.toml       # 项目配置(用于uv)
└── requirements.txt     # 项目依赖
``` 