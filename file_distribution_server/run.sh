#!/bin/bash

# 获取本机局域网IP地址
echo "正在启动服务器..."
echo "获取本机网络地址:"
IP=$(hostname -I | awk '{print $1}')

# 设置服务器IP和端口
export SERVER_HOST="$IP"
export SERVER_PORT="8001"

echo "  网页访问地址: http://$SERVER_HOST:$SERVER_PORT"
echo "  WebSocket访问地址: ws://$SERVER_HOST:$SERVER_PORT/ws/{client_id}"
echo ""
echo ""

# 启动服务器
uv run uvicorn main:app --reload --host 0.0.0.0 --port $SERVER_PORT
