import os
import uvicorn
from fastapi import FastAPI, File, UploadFile, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import gradio as gr
import hashlib
import time
import signal
import sys
import asyncio
from contextlib import asynccontextmanager

from ws_manager import ConnectionManager
from file_processor import FileProcessor

# 实例化WebSocket连接管理器
manager = ConnectionManager()

# 实例化文件处理器
file_processor = FileProcessor(upload_dir="static")

@asynccontextmanager
async def lifespan(app: FastAPI):
    # 启动事件
    print("应用已启动，准备接受连接")
    yield
    # 关闭事件
    print("应用正在关闭，清理资源...")
    await manager.disconnect_all()
    print("所有连接已关闭，资源已清理")

# 实例化FastAPI应用
app = FastAPI(title="文件分发服务器", lifespan=lifespan)

# 挂载静态文件目录
app.mount("/static", StaticFiles(directory="static"), name="static")

# WebSocket路由，用于ESP32S3设备连接
@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: str):
    await manager.connect(websocket, client_id)
    # 设备连接事件记录
    print(f"设备 {client_id} 已连接")
    
    try:
        while True:
            # 接收消息
            data = await websocket.receive_text()
            # 处理消息
            await manager.handle_message(client_id, data)
    except WebSocketDisconnect:
        manager.disconnect(client_id)
        # 设备断开事件记录
        print(f"设备 {client_id} 已断开连接")

# 文件上传接口
@app.post("/upload/{client_id}")
async def upload_file(client_id: str, file: UploadFile = File(...)):
    """
    上传文件并处理
    客户端A通过此接口上传文件
    """
    try:
        # 检查文件类型，只接受音频文件
        content_type = file.content_type
        if not content_type or not content_type.startswith("audio/"):
            raise HTTPException(status_code=400, detail="只接受音频文件格式")
        
        # 保存上传的文件
        file_path, filename = await file_processor.save_uploaded_file(file, file.filename)
        
        # 计算文件MD5
        md5_hash = hashlib.md5()
        with open(file_path, "rb") as f:
            for chunk in iter(lambda: f.read(4096), b""):
                md5_hash.update(chunk)
        file_md5 = md5_hash.hexdigest()
        
        # 获取文件大小
        file_size = os.path.getsize(file_path)
        
        # 如果指定的客户端在线，向其发送通知
        if client_id in manager.active_connections:
            # 构建下载信息
            download_info = {
                "filename": filename,
                "size": file_size,
                "md5": file_md5,
                "url": f"http://{os.environ.get('SERVER_HOST', 'localhost')}:8000/download/{filename}"
            }
            
            # 发送下载通知
            notification_sent = await manager.notify_client_to_download(client_id, download_info)
            
            if notification_sent:
                return {
                    "status": "success", 
                    "message": f"文件已上传并处理，已通知客户端 {client_id}", 
                    "filename": filename,
                    "md5": file_md5,
                    "size": file_size
                }
            else:
                return {
                    "status": "error", 
                    "message": f"文件已上传，但无法通知客户端 {client_id}"
                }
        else:
            return {
                "status": "warning", 
                "message": f"文件已上传并处理，但客户端 {client_id} 不在线", 
                "filename": filename
            }
    
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"文件处理失败: {str(e)}")

# 文件下载接口
@app.get("/download/{filename}")
async def download_file(filename: str):
    """
    下载文件接口
    客户端B收到通知后通过此接口下载文件
    """
    file_path = file_processor.get_file_path(filename)
    if os.path.exists(file_path):
        return FileResponse(file_path, filename=filename)
    else:
        raise HTTPException(status_code=404, detail="文件未找到")

# 获取活跃客户端列表
@app.get("/clients")
async def get_clients():
    """获取当前连接的客户端列表"""
    clients = manager.get_active_clients()
    return {"clients": clients}

# 获取客户端详细信息
@app.get("/clients/{client_id}")
async def get_client_details(client_id: str):
    """获取指定客户端的详细信息"""
    if client_id not in manager.active_connections:
        raise HTTPException(status_code=404, detail="客户端不在线")
    
    client_info = manager.device_info.get(client_id, {})
    client_files = manager.device_files.get(client_id, [])
    
    return {
        "client_id": client_id,
        "info": client_info,
        "files": client_files,
        "online": True
    }

# favicon.ico路由
@app.get("/favicon.ico")
async def favicon():
    """提供网站图标"""
    favicon_path = "static/favicon.ico"
    if os.path.exists(favicon_path):
        return FileResponse(favicon_path, media_type="image/x-icon")
    raise HTTPException(status_code=404, detail="Favicon not found")

# 创建Gradio界面
def create_gradio_interface():
    """创建Gradio界面"""
    
    def refresh_clients():
        """刷新客户端列表并返回下拉列表选项"""
        clients = manager.get_active_clients()
        if not clients:
            return gr.update(choices=[], value=None), "当前没有在线设备"
        
        # 只有一个设备时，自动选择该设备并获取信息
        if len(clients) == 1:
            client_id = clients[0]
            client_info = get_client_info(client_id)
            return gr.update(choices=clients, value=client_id), client_info
        
        # 多个设备时，更新选项列表
        return gr.update(choices=clients, value=clients[-1] if clients else None), f"已发现 {len(clients)} 个在线设备，请选择一个设备查看详情"
    
    def get_client_info(client_id):
        """获取客户端信息"""
        if not client_id or client_id == "当前没有在线设备":
            return "请先选择一个在线设备"
        
        client_info = manager.device_info.get(client_id, {})
        client_files = manager.device_files.get(client_id, [])
        
        info_text = f"设备ID: {client_id}\n"
        info_text += f"MAC地址: {client_info.get('mac', '未知')}\n"
        info_text += f"固件版本: {client_info.get('version', '未知')}\n"
        info_text += f"最后活跃: {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(client_info.get('last_seen', 0)))}\n"
        info_text += f"文件数量: {len(client_files)}\n\n"
        
        if client_files:
            info_text += "文件列表:\n"
            for file in client_files:
                info_text += f"- {file.get('filename')}: {file.get('size')} 字节, {file.get('md5')}\n"
        else:
            info_text += "设备没有报告文件"
        
        return info_text
    
    def upload_to_client(file, client_id):
        """上传文件到指定客户端"""
        if not client_id or client_id == "当前没有在线设备":
            return "请先选择一个在线设备", None
        
        if not file:
            return "请选择要上传的音频文件", None
        
        if client_id not in manager.active_connections:
            return f"设备 {client_id} 不在线", None
        
        # 构建文件上传URL (实际上传通过API接口完成)
        upload_url = f"/upload/{client_id}"
        
        return f"文件 {file.name} 将发送到客户端: {client_id}\n请使用API接口 {upload_url} 上传", file
    
    # 自定义CSS，避免加载特定字体
    custom_css = """
    * {
      font-family: system-ui, -apple-system, BlinkMacSystemFont, sans-serif !important;
    }
    code, pre {
      font-family: ui-monospace, Consolas, monospace !important;
    }
    """
    
    with gr.Blocks(title="文件分发服务器", css=custom_css) as interface:
        gr.Markdown("# 文件分发服务器")
        
        with gr.Row():
            with gr.Column(scale=1):
                refresh_btn = gr.Button("刷新设备列表", variant="primary")
                clients_dropdown = gr.Dropdown(label="在线设备", choices=[], value=None)
                client_info = gr.Textbox(label="设备信息", lines=10)
                
                # 刷新设备列表时自动获取设备信息
                refresh_btn.click(fn=refresh_clients, outputs=[clients_dropdown, client_info])
                
                # 选择设备时自动获取设备信息
                clients_dropdown.change(fn=get_client_info, inputs=clients_dropdown, outputs=client_info)
                
                # 页面加载时自动刷新一次设备列表
                interface.load(fn=refresh_clients, outputs=[clients_dropdown, client_info])
            
            with gr.Column(scale=2):
                # 音频文件上传组件
                file_input = gr.File(label="选择要上传的音频文件", file_types=["audio"])
                upload_btn = gr.Button("上传文件")
                result_box = gr.Textbox(label="结果")
                
                # 音频播放器组件
                audio_player = gr.Audio(label="音频预览", type="filepath", visible=False)
                
                # 上传按钮点击事件
                upload_btn.click(
                    fn=upload_to_client,
                    inputs=[file_input, clients_dropdown],
                    outputs=[result_box, audio_player]
                )
                
                # 文件上传后显示音频播放器
                def show_audio(file):
                    if file:
                        return gr.update(value=file, visible=True)
                    return gr.update(visible=False)
                
                file_input.change(fn=show_audio, inputs=file_input, outputs=audio_player)
    
    # 添加关闭检测
    interface.close_callback = lambda: print("Gradio界面已关闭")
    
    return interface

# 创建Gradio界面并挂载到FastAPI应用
gradio_app = create_gradio_interface()
app = gr.mount_gradio_app(app, gradio_app, path="/")



