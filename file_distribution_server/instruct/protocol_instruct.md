# 设备与服务器通信协议

## 1. 设备上线
### 请求格式
```json
{
  "type": "online",
  "data": {
    "version": "1.0.0",
    "mac": "AA:BB:CC:DD:EE:FF"
  }
}
```

### 响应格式
```json
{
  "type": "online_ack",
  "status": "success",
  "message": "设备已成功上线"
}
```

## 2. 获取设备文件列表
### 请求格式
```json
{
  "type": "file_list",
  "data": {
    "files": [
      {
        "filename": "file1.bin",
        "size": 1024,
        "md5": "d41d8cd98f00b204e9800998ecf8427e",
        "timestamp": 1623456789
      },
      {
        "filename": "file2.bin",
        "size": 2048,
        "md5": "c4ca4238a0b923820dcc509a6f75849b",
        "timestamp": 1623456790
      }
    ]
  }
}
```

### 响应格式
```json
{
  "type": "file_list_ack",
  "status": "success",
  "message": "文件列表已接收"
}
```

## 3. 通知设备下载文件
### 服务器发送格式
```json
{
  "type": "download_notify",
  "data": {
    "filename": "update.bin",
    "size": 4096,
    "md5": "e10adc3949ba59abbe56e057f20f883e",
    "url": "http://server_ip:8000/download/update.bin"
  }
}
```

### 设备确认格式
```json
{
  "type": "download_ack",
  "status": "success",
  "message": "开始下载文件",
  "data": {
    "filename": "update.bin"
  }
}
```

## 4. 设备下载完成通知
### 设备发送格式
```json
{
  "type": "download_complete",
  "status": "success",
  "data": {
    "filename": "update.bin",
    "md5": "e10adc3949ba59abbe56e057f20f883e"
  }
}
```

### 服务器确认格式
```json
{
  "type": "download_complete_ack",
  "status": "success",
  "message": "文件下载确认完成"
}
```

## 5. 心跳保活
### 请求格式
```json
{
  "type": "heartbeat",
  "timestamp": 1623456789
}
```

### 响应格式
```json
{
  "type": "heartbeat_ack",
  "timestamp": 1623456790
}
```

## 错误处理
所有通信中，如遇到错误，响应格式为：
```json
{
  "type": "error",
  "status": "error",
  "code": 1001,
  "message": "具体错误信息"
}
```

常见错误码：
- 1001: 通用错误
- 1002: 参数错误
- 1003: 文件不存在
- 1004: MD5校验失败
- 1005: 存储空间不足 