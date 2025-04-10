#!/bin/bash

# 获取本机局域网IP地址
echo "正在启动服务器..."
echo "获取本机网络地址:"
IP=$(hostname -I | awk '{print $1}')
echo "  网页访问地址: http://$IP:8001"
echo "  WebSocket访问地址: ws://$IP:8001/ws/{client_id}"
echo ""
echo "注意：使用WebSocket连接时，请将{client_id}替换为您的设备ID"
echo ""

# 启动服务器
uv run uvicorn main:app --reload --host 0.0.0.0 --port 8001
