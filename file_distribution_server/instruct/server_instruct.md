## 系统概述

本系统用于实现从 PC 客户端（客户端 A）上传文件至服务器进行处理，并将处理结果自动发送至 ESP32S3 设备（客户端 B）下载的功能。整个系统由 FastAPI 和 Gradio 共同构建，前后端不做分离。

---

## 系统环境

| 组件       | 描述                        |
|------------|-----------------------------|
| 客户端 A   | PC 端（Windows 或 macOS）   |
| 客户端 B   | ESP32S3 设备（嵌入式）       |
| 服务端     | Linux 系统，具有公网 IP     |
| 编程语言   | Python                      |
| 后端框架   | FastAPI搭配uvicorn                     |
| 前端框架   | Gradio（PC端）|
| 包管理器   | uv |
---

## 操作流程

1. 用户在浏览器访问服务器（Gradio 页面）。
2. 显示当前在线的 ESP32S3 设备。
3. 用户选择目标设备并建立连接。
4. 用户上传文件（由客户端 A 发起）。
5. 服务端对文件进行处理（如格式转换等）。
6. 文件处理完成后，通知目标客户端 B。
7. 客户端 B 接收到通知后，下载对应文件。

---

## 逻辑结构

### 服务端逻辑

#### 1. WebSocket 管理

- 客户端 B 使用 WebSocket 与服务器保持连接。
- 客户端 A 上传文件时不使用 WebSocket。
- 服务端维护所有已连接客户端 B 的映射信息（如 ID、连接对象）。

#### 2. 文件上传接口

```http
POST /upload/{client_id}
```

- 客户端 A 使用此接口上传文件。
- 文件将保存到服务器指定目录。
- 上传成功后，异步触发 `process_and_notify_b`。

#### 3. 文件处理逻辑

```python
async def process_and_notify_b(filename, client_id):
    # 对文件进行转换或处理
    # 查找目标 B 客户端的 WebSocket 连接
    # 通过 WebSocket 发送 JSON 通知：
    # {"status": "ready", "filename": "processed_xxx.txt"}
```

- 支持异步处理，避免阻塞上传请求。
- 文件命名规则可加上 `processed_` 前缀或 UUID。

#### 4. 文件下载接口

```http
GET /download/{filename}
```

- 客户端 B 在收到通知后调用该接口下载文件。
- 返回处理完成的文件内容。

---

### 客户端逻辑

#### 客户端 A（PC）

- 用户通过浏览器上传文件。
- 通过 `POST /upload/{client_id}` 上传至服务器。

#### 客户端 B（ESP32S3）

- 启动后连接服务器 WebSocket：

```text
ws://<server_ip>:<port>/ws/{client_id}
```

- 等待接收 JSON 通知格式如下：

```json
{
  "status": "ready",
  "filename": "processed_xxx.txt"
}
```

- 收到通知后，请求 `/download/{filename}` 下载文件。

---

## 前端界面（Gradio）

- 显示在线设备列表（由服务端维护 WebSocket 连接状态）。
- 文件上传组件。
- 设备连接按钮。

---

## 项目结构建议

```
project/
│
├── main.py                  # FastAPI + Gradio 主入口
├── ws_manager.py            # WebSocket 连接管理
├── file_processor.py        # 文件处理逻辑
├── static/                  # 上传和处理后文件的存储目录
├── templates/               # 可选：Gradio 自定义模板
└── requirements.txt         # 项目依赖(pip)
└── pyproject.toml           # 项目依赖(uv)
```