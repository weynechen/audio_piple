import asyncio
import json
import time
import hashlib
import aiohttp
import random
import argparse
import logging
from typing import Dict, List, Optional


class DeviceClientSimulator:
    """设备客户端模拟器"""
    
    def __init__(
        self, 
        server_url: str, 
        device_id: str, 
        mac_address: Optional[str] = None,
        version: str = "1.0.0",
        heartbeat_interval: int = 30
    ):
        self.server_url = server_url
        self.device_id = device_id
        self.mac_address = mac_address or f"{':'.join([f'{random.randint(0, 255):02X}' for _ in range(6)])}"
        self.version = version
        self.heartbeat_interval = heartbeat_interval
        self.files: List[Dict] = []
        self.websocket = None
        self.is_connected = False
        
        # 配置日志
        self.logger = self._setup_logger()
    
    def _setup_logger(self):
        """设置日志"""
        logger = logging.getLogger(f"Device-{self.device_id}")
        logger.setLevel(logging.INFO)
        
        handler = logging.StreamHandler()
        formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')
        handler.setFormatter(formatter)
        
        logger.addHandler(handler)
        return logger
    
    def _get_mock_files(self) -> List[Dict]:
        """生成模拟的文件列表"""
        mock_files = []
        for i in range(1, random.randint(2, 5)):
            filename = f"file{i}.bin"
            size = random.randint(1024, 10240)
            timestamp = int(time.time()) - random.randint(0, 86400)
            
            # 生成假的MD5
            md5 = hashlib.md5(f"{filename}-{size}-{timestamp}".encode()).hexdigest()
            
            mock_files.append({
                "filename": filename,
                "size": size,
                "md5": md5,
                "timestamp": timestamp
            })
        
        return mock_files
    
    async def _calculate_file_md5(self, file_path: str) -> str:
        """计算文件的MD5值"""
        try:
            md5_hash = hashlib.md5()
            with open(file_path, "rb") as f:
                for chunk in iter(lambda: f.read(4096), b""):
                    md5_hash.update(chunk)
            return md5_hash.hexdigest()
        except Exception as e:
            self.logger.error(f"计算MD5失败: {str(e)}")
            return ""
    
    async def connect(self):
        """连接到服务器"""
        try:
            session = aiohttp.ClientSession()
            self.websocket = await session.ws_connect(f"{self.server_url}/ws/{self.device_id}")
            self.is_connected = True
            self.logger.info(f"设备 {self.device_id} 已连接到服务器")
            
            # 发送设备上线消息
            await self.send_online_message()
            
            # 启动心跳任务
            asyncio.create_task(self.heartbeat_task())
            
            # 启动主消息处理循环
            await self.message_loop()
            
        except Exception as e:
            self.logger.error(f"连接失败: {str(e)}")
            self.is_connected = False
            if hasattr(self, 'websocket') and self.websocket:
                await self.websocket.close()
    
    async def send_online_message(self):
        """发送设备上线消息"""
        message = {
            "type": "online",
            "data": {
                "version": self.version,
                "mac": self.mac_address
            }
        }
        await self.send_message(message)
        self.logger.info("已发送设备上线消息")
    
    async def send_file_list(self):
        """发送设备文件列表"""
        # 生成模拟文件列表
        self.files = self._get_mock_files()
        
        message = {
            "type": "file_list",
            "data": {
                "files": self.files
            }
        }
        await self.send_message(message)
        self.logger.info(f"已发送设备文件列表，共 {len(self.files)} 个文件")
    
    async def download_file(self, filename: str, file_url: str, expected_md5: str, file_size: int):
        """下载文件"""
        self.logger.info(f"开始下载文件: {filename}, 大小: {file_size} 字节")
        
        # 发送下载确认
        ack_message = {
            "type": "download_ack",
            "status": "success",
            "message": "开始下载文件",
            "data": {
                "filename": filename
            }
        }
        await self.send_message(ack_message)
        
        try:
            # 模拟下载
            async with aiohttp.ClientSession() as session:
                async with session.get(file_url) as response:
                    if response.status != 200:
                        self.logger.error(f"下载失败，HTTP状态码: {response.status}")
                        return
                    
                    # 保存文件到本地
                    save_path = f"downloaded_{filename}"
                    with open(save_path, "wb") as f:
                        while True:
                            chunk = await response.content.read(8192)
                            if not chunk:
                                break
                            f.write(chunk)
            
            # 校验MD5
            calculated_md5 = await self._calculate_file_md5(save_path)
            if calculated_md5 and calculated_md5 == expected_md5:
                # 发送下载完成消息
                complete_message = {
                    "type": "download_complete",
                    "status": "success",
                    "data": {
                        "filename": filename,
                        "md5": calculated_md5
                    }
                }
                await self.send_message(complete_message)
                self.logger.info(f"文件 {filename} 下载完成，MD5校验通过")
            else:
                # 发送错误消息
                error_message = {
                    "type": "error",
                    "status": "error",
                    "code": 1004,
                    "message": f"MD5校验失败: 期望 {expected_md5}，实际 {calculated_md5}"
                }
                await self.send_message(error_message)
                self.logger.error(f"文件 {filename} MD5校验失败")
                
        except Exception as e:
            self.logger.error(f"下载文件时出错: {str(e)}")
            # 发送错误消息
            error_message = {
                "type": "error",
                "status": "error",
                "code": 1001,
                "message": f"下载文件出错: {str(e)}"
            }
            await self.send_message(error_message)
    
    async def heartbeat_task(self):
        """心跳任务"""
        while self.is_connected:
            try:
                message = {
                    "type": "heartbeat",
                    "timestamp": int(time.time())
                }
                await self.send_message(message)
                self.logger.debug("已发送心跳消息")
                
                # 等待下一次心跳
                await asyncio.sleep(self.heartbeat_interval)
            except Exception as e:
                self.logger.error(f"心跳任务出错: {str(e)}")
                break
    
    async def send_message(self, message: dict):
        """发送消息"""
        if not self.is_connected or not self.websocket:
            self.logger.error("无法发送消息，未连接到服务器")
            return
        
        try:
            await self.websocket.send_json(message)
        except Exception as e:
            self.logger.error(f"发送消息失败: {str(e)}")
            self.is_connected = False
    
    async def message_loop(self):
        """消息处理循环"""
        if not self.is_connected or not self.websocket:
            return
        
        try:
            # 发送文件列表
            await asyncio.sleep(1)  # 等待连接稳定
            await self.send_file_list()
            
            # 处理接收到的消息
            async for msg in self.websocket:
                if msg.type == aiohttp.WSMsgType.TEXT:
                    await self.handle_message(msg.data)
                elif msg.type == aiohttp.WSMsgType.ERROR:
                    self.logger.error(f"WebSocket错误: {self.websocket.exception()}")
                    break
                elif msg.type == aiohttp.WSMsgType.CLOSED:
                    self.logger.info("WebSocket连接已关闭")
                    break
        except Exception as e:
            self.logger.error(f"消息循环出错: {str(e)}")
        finally:
            self.is_connected = False
            if hasattr(self, 'websocket') and self.websocket:
                await self.websocket.close()
            self.logger.info("已断开连接")
    
    async def handle_message(self, message_data: str):
        """处理接收到的消息"""
        try:
            message = json.loads(message_data) if isinstance(message_data, str) else message_data
            message_type = message.get("type")
            
            self.logger.info(f"收到消息类型: {message_type}")
            
            if message_type == "online_ack":
                self.logger.info(f"设备上线确认: {message.get('message')}")
            
            elif message_type == "file_list_ack":
                self.logger.info(f"文件列表确认: {message.get('message')}")
            
            elif message_type == "download_notify":
                # 收到下载通知
                data = message.get("data", {})
                filename = data.get("filename")
                file_url = data.get("url")
                expected_md5 = data.get("md5")
                file_size = data.get("size")
                
                self.logger.info(f"收到下载通知: {filename}")
                
                # 启动下载任务
                asyncio.create_task(self.download_file(
                    filename, file_url, expected_md5, file_size
                ))
            
            elif message_type == "download_complete_ack":
                self.logger.info(f"下载完成确认: {message.get('message')}")
            
            elif message_type == "heartbeat_ack":
                self.logger.debug(f"心跳确认: {message.get('timestamp')}")
            
            elif message_type == "error":
                self.logger.error(f"收到错误消息: {message.get('message')}, 代码: {message.get('code')}")
            
            else:
                self.logger.warning(f"未知消息类型: {message_type}")
                
        except json.JSONDecodeError:
            self.logger.error(f"消息格式错误: {message_data}")
        except Exception as e:
            self.logger.error(f"处理消息时出错: {str(e)}")


async def main():
    """主函数"""
    parser = argparse.ArgumentParser(description="设备客户端模拟器")
    parser.add_argument("--server", default="ws://localhost:8001", help="服务器URL")
    parser.add_argument("--id", default=f"dev-{random.randint(1000, 9999)}", help="设备ID")
    parser.add_argument("--mac", default=None, help="MAC地址")
    parser.add_argument("--version", default="1.0.0", help="设备版本")
    parser.add_argument("--heartbeat", type=int, default=30, help="心跳间隔(秒)")
    
    args = parser.parse_args()
    
    # 创建并启动客户端
    client = DeviceClientSimulator(
        server_url=args.server,
        device_id=args.id,
        mac_address=args.mac,
        version=args.version,
        heartbeat_interval=args.heartbeat
    )
    
    await client.connect()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("程序已被用户中断") 