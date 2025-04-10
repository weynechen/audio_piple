import os
import uvicorn
from fastapi import FastAPI, File, UploadFile, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
import gradio as gr
from gradio.components import Timer
import hashlib
import time
import signal
import sys
import asyncio
from contextlib import asynccontextmanager
import io

from ws_manager import ConnectionManager
from file_processor import FileProcessor

# 实例化WebSocket连接管理器
manager = ConnectionManager()
# 设置心跳超时时间（秒）
manager.heartbeat_timeout = int(os.environ.get('HEARTBEAT_TIMEOUT', '10'))
print(f"心跳超时设置为: {manager.heartbeat_timeout}秒")

# 实例化文件处理器
file_processor = FileProcessor(upload_dir="static")

# 全局变量，用于存储传输进度
global_transfer_progress = {"percent": 0, "status": "空闲", "filename": ""}

# 进度更新回调函数
async def update_transfer_progress(progress_data):
    """更新全局传输进度变量"""
    global global_transfer_progress
    global_transfer_progress.update(progress_data)
    print(f"进度更新: {progress_data['percent']}% - {progress_data['status']} - {progress_data['filename']}")

# 设置传输进度回调
manager.set_transfer_progress_callback(update_transfer_progress)

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
    return await upload_file_impl(client_id, file)

async def upload_file_impl(client_id: str, file: UploadFile):
    """
    上传文件并处理的实现逻辑
    """
    try:
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
            server_host = os.environ.get('SERVER_HOST', 'localhost')
            server_port = os.environ.get('SERVER_PORT', '8000')
            download_info = {
                "filename": filename,
                "size": file_size,
                "md5": file_md5,
                "url": f"http://{server_host}:{server_port}/download/{filename}"
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
        print(f"文件上传处理错误: {str(e)}")
        import traceback
        traceback.print_exc()
        return {
            "status": "error", 
            "message": f"文件处理失败: {str(e)}"
        }

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
    
    # 使用全局变量进度数据
    global global_transfer_progress
    
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
    
    async def upload_to_client(file, client_id):
        """上传文件到指定客户端"""
        global global_transfer_progress
        
        if not client_id or client_id == "当前没有在线设备":
            global_transfer_progress["status"] = "错误"
            global_transfer_progress["percent"] = 0
            global_transfer_progress["filename"] = ""
            return gr.update(value=0, label="进度: 错误 - 请先选择一个在线设备"), None
        
        if not file:
            global_transfer_progress["status"] = "错误"
            global_transfer_progress["percent"] = 0
            global_transfer_progress["filename"] = ""
            return gr.update(value=0, label="进度: 错误 - 请选择要上传的文件"), None
        
        if client_id not in manager.active_connections:
            global_transfer_progress["status"] = "错误"
            global_transfer_progress["percent"] = 0
            global_transfer_progress["filename"] = ""
            return gr.update(value=0, label="进度: 错误 - 设备不在线"), None
        
        try:
            # 重置并初始化进度
            global_transfer_progress["percent"] = 0
            global_transfer_progress["status"] = "正在准备"
            global_transfer_progress["filename"] = os.path.basename(file.name)
            
            # 创建上传文件对象
            file_path = file.name
            file_name = os.path.basename(file_path)
            
            # 更新进度状态为准备上传
            global_transfer_progress["status"] = "正在上传"
            
            with open(file_path, "rb") as f:
                file_content = f.read()
            
            # 更新进度为读取完成
            global_transfer_progress["status"] = "读取完成"
            
            # 创建UploadFile对象
            upload_file = UploadFile(
                filename=file_name,
                file=io.BytesIO(file_content),
                size=len(file_content)
            )
            
            # 更新进度为处理中
            global_transfer_progress["status"] = "处理中"
            
            # 调用上传文件接口
            result = await upload_file_impl(client_id, upload_file)
            
            # 更新进度为完成
            global_transfer_progress["status"] = "完成" if result.get("status") == "success" else "失败"
            
            # 返回进度条更新
            return gr.update(
                value=global_transfer_progress["percent"], 
                label=f"进度: {global_transfer_progress['status']} - {file_name}"
            ), None
            
        except Exception as e:
            import traceback
            traceback.print_exc()
            global_transfer_progress["status"] = "错误"
            global_transfer_progress["percent"] = 0
            return gr.update(
                value=0, 
                label=f"进度: 错误 - {str(e)}"
            ), file
    
    def show_audio(file):
        """显示音频预览"""
        if file and hasattr(file, 'name'):
            file_ext = file.name.split('.')[-1].lower() if '.' in file.name else ''
            audio_extensions = ['mp3', 'wav', 'ogg', 'flac']
            if file_ext in audio_extensions:
                return gr.update(value=file)
        return gr.update(value=None)
    
    # 自定义CSS，避免加载特定字体，同时美化进度条样式
    custom_css = """
    * {
      font-family: system-ui, -apple-system, BlinkMacSystemFont, sans-serif !important;
    }
    code, pre {
      font-family: ui-monospace, Consolas, monospace !important;
    }
    .progress-bar-container {
      margin-top: 10px;
      margin-bottom: 10px;
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
                gr.Markdown("### 文件上传")
                
                # 添加进度条组件
                progress_bar = gr.Slider(
                    minimum=0, 
                    maximum=100, 
                    value=0, 
                    label="进度: 空闲", 
                    interactive=False,
                    elem_classes="progress-bar-container"
                )
                
                with gr.Row():
                    file_upload = gr.File(label="选择要上传的文件")
                    upload_button = gr.Button("上传到设备", variant="primary")
                
                # 添加Timer组件实现自动刷新进度条
                with gr.Row(visible=False):  # 隐藏Timer组件
                    # 默认状态为不活动
                    timer = Timer(1, active=False)  # 每1秒触发一次，默认不活动
                    # 添加上传超时计数器
                    upload_timeout_counter = gr.State(value=0)
                    # 添加上一次进度值记录
                    last_progress_value = gr.State(value=0)
                
                # 更新上传进度和控制按钮状态的函数
                def update_progress_and_button(timer_value=None, timeout_counter=0, last_progress=0):
                    """更新进度条状态，控制按钮状态，并在进度完成或超时时停止Timer"""
                    global global_transfer_progress
                    
                    # 获取当前上传的客户端ID
                    current_client_id = clients_dropdown.value
                    
                    # 获取当前进度值
                    current_progress = global_transfer_progress["percent"]
                    
                    # 检查是否完成(100%)或出错状态
                    is_complete = (current_progress >= 100 or 
                                global_transfer_progress["status"] in ["完成", "错误", "失败"])
                    
                    # 检查进度是否有更新
                    has_progress_update = current_progress != last_progress
                    
                    # 只有在没有进度更新时才增加超时计数
                    if not has_progress_update and not is_complete:
                        new_timeout_counter = timeout_counter + 1
                    else:
                        # 有进度更新，重置超时计数
                        new_timeout_counter = 0
                    
                    # 检查是否超时(10秒)
                    is_timeout = new_timeout_counter > 10
                    
                    # 检查设备是否断开连接
                    device_disconnected = (current_client_id and 
                                          current_client_id not in manager.active_connections and
                                          global_transfer_progress["status"] not in ["空闲", "完成", "错误", "失败", "超时"])
                    
                    if is_complete or is_timeout or device_disconnected:
                        # 如果完成、超时或设备断开，重置状态并启用按钮
                        if is_timeout:
                            global_transfer_progress["status"] = "超时"
                            global_transfer_progress["percent"] = 0
                        elif device_disconnected:
                            global_transfer_progress["status"] = "设备断开"
                            global_transfer_progress["percent"] = 0
                        
                        return (
                            gr.update(value=global_transfer_progress["percent"], 
                                    label=f"进度: {global_transfer_progress['status']} - {global_transfer_progress['filename']}"),
                            gr.update(active=False),
                            0,
                            gr.update(interactive=True),
                            0  # 重置last_progress_value
                        )
                    else:
                        # 继续更新但不停止Timer，保持按钮禁用
                        return (
                            gr.update(value=global_transfer_progress["percent"], 
                                    label=f"进度: {global_transfer_progress['status']} - {global_transfer_progress['filename']}"),
                            gr.update(active=True),
                            new_timeout_counter,
                            gr.update(interactive=False),
                            current_progress  # 更新last_progress_value
                        )
                
                # 上传按钮点击事件，增加更新进度条和激活Timer，禁用按钮
                upload_button.click(
                    fn=upload_to_client,
                    inputs=[file_upload, clients_dropdown],
                    outputs=[progress_bar, file_upload],
                    api_name="upload_file_to_client"
                ).then(
                    fn=lambda: (gr.update(active=True), 0, gr.update(interactive=False), 0),
                    inputs=None,
                    outputs=[timer, upload_timeout_counter, upload_button, last_progress_value]
                )
                
                # 使用Timer组件的tick事件触发进度条更新和按钮状态控制
                timer.tick(
                    fn=update_progress_and_button,
                    inputs=[timer, upload_timeout_counter, last_progress_value],
                    outputs=[progress_bar, timer, upload_timeout_counter, upload_button, last_progress_value]
                )
                
                # 添加自动刷新状态提示
                gr.Markdown("""
                > **提示**: 进度条每秒自动更新一次，显示最新传输进度。
                """)
                
                # 音频播放器（如果需要预览）
                with gr.Accordion("音频预览", open=True):
                    audio_player = gr.Audio(label="音频预览", type="filepath")
                
                # 文件上传后如果是音频文件，预览
                file_upload.change(
                    fn=show_audio,
                    inputs=[file_upload],
                    outputs=[audio_player]
                )
    
    # 添加关闭检测
    interface.close_callback = lambda: print("Gradio界面已关闭")
    
    return interface

# 创建Gradio界面并挂载到FastAPI应用
gradio_app = create_gradio_interface()
app = gr.mount_gradio_app(app, gradio_app, path="/")



