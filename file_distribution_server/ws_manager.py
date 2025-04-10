from fastapi import WebSocket
from typing import Dict, List
import json
import time
import asyncio


class ConnectionManager:
    """WebSocket Connection Manager Class"""
    
    def __init__(self):
        # Dictionary to store each client ID and its corresponding WebSocket connection
        self.active_connections: Dict[str, WebSocket] = {}
        # Store device information
        self.device_info: Dict[str, Dict] = {}
        # Store device file list
        self.device_files: Dict[str, List[Dict]] = {}
        # 心跳超时时间（秒）
        self.heartbeat_timeout = 10
        # 启动心跳检测任务
        self.heartbeat_check_task = None
        # 传输进度回调
        self.transfer_progress_callback = None
    
    async def connect(self, websocket: WebSocket, client_id: str):
        """Establish new WebSocket connection"""
        await websocket.accept()
        self.active_connections[client_id] = websocket
        # 初始化设备信息，记录最后心跳时间
        if client_id not in self.device_info:
            self.device_info[client_id] = {"last_seen": time.time()}
        else:
            self.device_info[client_id]["last_seen"] = time.time()
        
        # 确保心跳检测任务已启动
        if self.heartbeat_check_task is None or self.heartbeat_check_task.done():
            self.heartbeat_check_task = asyncio.create_task(self.check_heartbeats())
            
        print(f"Client {client_id} connected, current connections: {len(self.active_connections)}")
    
    def disconnect(self, client_id: str):
        """Disconnect WebSocket connection"""
        if client_id in self.active_connections:
            del self.active_connections[client_id]
            print(f"Client {client_id} disconnected, current connections: {len(self.active_connections)}")
    
    async def send_message(self, client_id: str, message: dict):
        """Send message to specified client"""
        if client_id in self.active_connections:
            try:
                websocket = self.active_connections[client_id]
                print(f"正在向客户端 {client_id} 发送消息: {message.get('type', 'unknown')}")
                await websocket.send_json(message)
                print(f"成功向客户端 {client_id} 发送消息")
                return True
            except Exception as e:
                print(f"向客户端 {client_id} 发送消息时出错: {str(e)}")
                return False
        else:
            print(f"客户端 {client_id} 不在线，无法发送消息")
            return False
    
    async def handle_message(self, client_id: str, message_data: str):
        """Handle message received from client"""
        try:
            message = json.loads(message_data) if isinstance(message_data, str) else message_data
            message_type = message.get("type")
            
            print(f"Received message type: {message_type} from client {client_id}")
            
            # 无论是什么类型的消息，都更新最后活动时间
            if client_id in self.device_info:
                self.device_info[client_id]["last_seen"] = time.time()
            
            if message_type == "online":
                # Handle device online message
                data = message.get("data", {})
                self.device_info[client_id] = {
                    "version": data.get("version"),
                    "mac": data.get("mac"),
                    "last_seen": time.time()
                }
                
                # Send confirmation response
                response = {
                    "type": "online_ack",
                    "status": "success",
                    "message": "Device successfully online"
                }
                await self.send_message(client_id, response)
                
            elif message_type == "file_list":
                # Handle device file list
                data = message.get("data", {})
                self.device_files[client_id] = data.get("files", [])
                
                # Send confirmation response
                response = {
                    "type": "file_list_ack",
                    "status": "success",
                    "message": "File list received"
                }
                await self.send_message(client_id, response)
                
            elif message_type == "download_ack":
                # Handle download confirmation
                print(f"Client {client_id} confirmed starting download file: {message.get('data', {}).get('filename')}")
                
                # 更新进度为开始下载
                if self.transfer_progress_callback:
                    await self.transfer_progress_callback({
                        "percent": 10,
                        "status": "开始下载",
                        "filename": message.get('data', {}).get('filename', '未知文件')
                    })
                
            elif message_type == "download_progress":
                # 处理下载进度通知
                data = message.get("data", {})
                filename = data.get("filename", "未知文件")
                percent = data.get("percent", 0)
                transferred = data.get("transferred", 0)
                total_size = data.get("total_size", 0)
                
                print(f"Client {client_id} download progress: {filename} - {percent}% ({transferred}/{total_size} bytes)")
                
                # 调用进度回调
                if self.transfer_progress_callback:
                    await self.transfer_progress_callback({
                        "percent": percent,
                        "status": "下载中",
                        "filename": filename
                    })
                
            elif message_type == "download_complete":
                # Handle download completion notification
                data = message.get("data", {})
                filename = data.get("filename")
                md5 = data.get("md5")
                
                print(f"Client {client_id} completed downloading file: {filename}, MD5: {md5}")
                
                # 更新进度为完成
                if self.transfer_progress_callback:
                    await self.transfer_progress_callback({
                        "percent": 100,
                        "status": "下载完成",
                        "filename": filename
                    })
                
                # Send confirmation response
                response = {
                    "type": "download_complete_ack",
                    "status": "success",
                    "message": "File download confirmation completed"
                }
                await self.send_message(client_id, response)
                
            elif message_type == "upload_ack":
                # Handle upload acknowledgement
                print(f"Client {client_id} confirmed starting upload file: {message.get('data', {}).get('filename')}")
                
                # 更新进度为开始上传
                if self.transfer_progress_callback:
                    await self.transfer_progress_callback({
                        "percent": 10,
                        "status": "开始上传",
                        "filename": message.get('data', {}).get('filename', '未知文件')
                    })
                
            elif message_type == "upload_progress":
                # 处理上传进度通知
                data = message.get("data", {})
                filename = data.get("filename", "未知文件")
                percent = data.get("percent", 0)
                transferred = data.get("transferred", 0)
                total_size = data.get("total_size", 0)
                
                print(f"Client {client_id} upload progress: {filename} - {percent}% ({transferred}/{total_size} bytes)")
                
                # 调用进度回调
                if self.transfer_progress_callback:
                    await self.transfer_progress_callback({
                        "percent": percent,
                        "status": "上传中",
                        "filename": filename
                    })
                
            elif message_type == "upload_complete":
                # Handle upload completion notification
                data = message.get("data", {})
                filename = data.get("filename")
                md5 = data.get("md5")
                
                print(f"Client {client_id} completed uploading file: {filename}, MD5: {md5}")
                
                # 更新进度为完成
                if self.transfer_progress_callback:
                    await self.transfer_progress_callback({
                        "percent": 100,
                        "status": "上传完成",
                        "filename": filename
                    })
                
                # 更新设备文件列表
                if client_id in self.device_files:
                    # 检查文件是否已在列表中
                    existing_file = False
                    for file_info in self.device_files[client_id]:
                        if file_info.get("filename") == filename:
                            file_info["md5"] = md5
                            file_info["timestamp"] = int(time.time())
                            existing_file = True
                            break
                    
                    # 如果文件不在列表中，添加它
                    if not existing_file and self.device_files[client_id] is not None:
                        self.device_files[client_id].append({
                            "filename": filename,
                            "md5": md5,
                            "timestamp": int(time.time())
                        })
                else:
                    self.device_files[client_id] = [{
                        "filename": filename,
                        "md5": md5,
                        "timestamp": int(time.time())
                    }]
                
                # Send confirmation response
                response = {
                    "type": "upload_complete_ack",
                    "status": "success",
                    "message": "File upload confirmation completed"
                }
                await self.send_message(client_id, response)
                
            elif message_type == "heartbeat":
                # Handle heartbeat message
                timestamp = message.get("timestamp")
                if client_id in self.device_info:
                    self.device_info[client_id]["last_seen"] = time.time()
                
                # Send heartbeat confirmation
                response = {
                    "type": "heartbeat_ack",
                    "timestamp": int(time.time())
                }
                await self.send_message(client_id, response)
                
            elif message_type == "error":
                # Handle error message
                code = message.get("code")
                error_message = message.get("message")
                print(f"Client {client_id} reported error: {error_message}, code: {code}")
                
            else:
                print(f"Unknown message type {message_type} from client {client_id}")
                
        except json.JSONDecodeError:
            print(f"Message format error: {message_data}")
        except Exception as e:
            print(f"Error processing message: {str(e)}")
    
    def get_active_clients(self) -> List[str]:
        """Get list of all active client IDs"""
        return list(self.active_connections.keys())
    
    async def notify_client_to_download(self, client_id: str, file_info: dict) -> bool:
        """Notify client to download file"""
        if client_id not in self.active_connections:
            print(f"无法通知客户端下载文件：客户端 {client_id} 不在线")
            print(f"当前在线客户端: {list(self.active_connections.keys())}")
            return False
        
        message = {
            "type": "download_notify",
            "data": file_info
        }
        
        print(f"正在通知客户端 {client_id} 下载文件: {file_info['filename']}")
        print(f"通知内容: {message}")
        
        # 设置进度为准备下载
        if self.transfer_progress_callback:
            await self.transfer_progress_callback({
                "percent": 5,
                "status": "准备下载",
                "filename": file_info['filename']
            })
        
        success = await self.send_message(client_id, message)
        
        if success:
            print(f"已成功通知客户端 {client_id} 下载文件")
        else:
            print(f"通知客户端 {client_id} 下载文件失败")
            if self.transfer_progress_callback:
                await self.transfer_progress_callback({
                    "percent": 0,
                    "status": "通知失败",
                    "filename": file_info['filename']
                })
            
        return success
    
    async def notify_client_to_upload(self, client_id: str, file_info: dict) -> bool:
        """Request client to upload a file"""
        if client_id not in self.active_connections:
            print(f"无法请求客户端上传文件：客户端 {client_id} 不在线")
            print(f"当前在线客户端: {list(self.active_connections.keys())}")
            return False
        
        message = {
            "type": "upload_request",
            "data": file_info
        }
        
        print(f"正在请求客户端 {client_id} 上传文件: {file_info['filename']}")
        print(f"请求内容: {message}")
        
        # 设置进度为准备上传
        if self.transfer_progress_callback:
            await self.transfer_progress_callback({
                "percent": 5,
                "status": "准备上传",
                "filename": file_info['filename']
            })
        
        success = await self.send_message(client_id, message)
        
        if success:
            print(f"已成功请求客户端 {client_id} 上传文件")
        else:
            print(f"请求客户端 {client_id} 上传文件失败")
            if self.transfer_progress_callback:
                await self.transfer_progress_callback({
                    "percent": 0,
                    "status": "请求失败",
                    "filename": file_info['filename']
                })
            
        return success
    
    async def check_heartbeats(self):
        """定期检查客户端心跳，断开超时连接"""
        while True:
            try:
                current_time = time.time()
                disconnected_clients = []
                
                # 检查所有客户端的最后活动时间
                for client_id in list(self.active_connections.keys()):
                    last_seen = self.device_info.get(client_id, {}).get("last_seen", 0)
                    # 如果超过心跳超时时间未收到消息，则视为断开
                    if current_time - last_seen > self.heartbeat_timeout:
                        print(f"客户端 {client_id} 心跳超时，最后活动: {int(current_time - last_seen)}秒前")
                        disconnected_clients.append(client_id)
                
                # 断开超时连接
                for client_id in disconnected_clients:
                    try:
                        websocket = self.active_connections[client_id]
                        await websocket.close(code=1001, reason="Heartbeat timeout")
                    except Exception as e:
                        print(f"关闭连接出错: {str(e)}")
                    finally:
                        self.disconnect(client_id)
                        print(f"已断开超时客户端: {client_id}")
                
                # 每3秒检查一次
                await asyncio.sleep(3)
            except Exception as e:
                print(f"心跳检查任务出错: {str(e)}")
                await asyncio.sleep(5)  # 出错后等待稍长时间再重试
    
    def set_transfer_progress_callback(self, callback):
        """设置传输进度回调函数"""
        self.transfer_progress_callback = callback
    
    async def disconnect_all(self):
        """Close all WebSocket connections"""
        # 取消心跳检查任务
        if self.heartbeat_check_task and not self.heartbeat_check_task.done():
            self.heartbeat_check_task.cancel()
            try:
                await self.heartbeat_check_task
            except asyncio.CancelledError:
                pass
        
        for client_id in list(self.active_connections.keys()):
            try:
                websocket = self.active_connections[client_id]
                await websocket.close(code=1000, reason="Server shutdown")
                self.disconnect(client_id)
            except Exception as e:
                print(f"Error closing connection for client {client_id}: {str(e)}")
        
        print(f"All connections disconnected, current connections: {len(self.active_connections)}")
        return True 