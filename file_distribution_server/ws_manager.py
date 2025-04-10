from fastapi import WebSocket
from typing import Dict, List
import json
import time


class ConnectionManager:
    """WebSocket Connection Manager Class"""
    
    def __init__(self):
        # Dictionary to store each client ID and its corresponding WebSocket connection
        self.active_connections: Dict[str, WebSocket] = {}
        # Store device information
        self.device_info: Dict[str, Dict] = {}
        # Store device file list
        self.device_files: Dict[str, List[Dict]] = {}
    
    async def connect(self, websocket: WebSocket, client_id: str):
        """Establish new WebSocket connection"""
        await websocket.accept()
        self.active_connections[client_id] = websocket
        print(f"Client {client_id} connected, current connections: {len(self.active_connections)}")
    
    def disconnect(self, client_id: str):
        """Disconnect WebSocket connection"""
        if client_id in self.active_connections:
            del self.active_connections[client_id]
            print(f"Client {client_id} disconnected, current connections: {len(self.active_connections)}")
    
    async def send_message(self, client_id: str, message: dict):
        """Send message to specified client"""
        if client_id in self.active_connections:
            websocket = self.active_connections[client_id]
            await websocket.send_json(message)
            return True
        return False
    
    async def handle_message(self, client_id: str, message_data: str):
        """Handle message received from client"""
        try:
            message = json.loads(message_data) if isinstance(message_data, str) else message_data
            message_type = message.get("type")
            
            print(f"Received message type: {message_type} from client {client_id}")
            
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
                
            elif message_type == "download_complete":
                # Handle download completion notification
                data = message.get("data", {})
                filename = data.get("filename")
                md5 = data.get("md5")
                
                print(f"Client {client_id} completed downloading file: {filename}, MD5: {md5}")
                
                # Send confirmation response
                response = {
                    "type": "download_complete_ack",
                    "status": "success",
                    "message": "File download confirmation completed"
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
            return False
        
        message = {
            "type": "download_notify",
            "data": file_info
        }
        
        return await self.send_message(client_id, message)
    
    async def disconnect_all(self):
        """Close all WebSocket connections"""
        for client_id in list(self.active_connections.keys()):
            try:
                websocket = self.active_connections[client_id]
                await websocket.close(code=1000, reason="Server shutdown")
                self.disconnect(client_id)
            except Exception as e:
                print(f"Error closing connection for client {client_id}: {str(e)}")
        
        print(f"All connections disconnected, current connections: {len(self.active_connections)}")
        return True 