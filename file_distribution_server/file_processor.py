import os
import uuid
from pathlib import Path
from typing import Tuple


class FileProcessor:
    """文件处理类"""
    
    def __init__(self, upload_dir: str = "static"):
        self.upload_dir = upload_dir
        # 确保上传目录存在
        os.makedirs(upload_dir, exist_ok=True)
    
    async def save_uploaded_file(self, file, original_filename: str) -> Tuple[str, str]:
        """
        保存上传的文件并返回文件存储路径和生成的文件名
        """
        # 生成唯一文件名
        file_extension = original_filename.split('.')[-1] if '.' in original_filename else ''
        unique_filename = f"{uuid.uuid4().hex}.{file_extension}" if file_extension else f"{uuid.uuid4().hex}"
        
        # 构建文件路径
        file_path = os.path.join(self.upload_dir, unique_filename)
        
        # 保存文件
        with open(file_path, "wb") as buffer:
            buffer.write(await file.read())
        
        return file_path, unique_filename
    
    async def process_file(self, file_path: str, filename: str) -> Tuple[str, str]:
        """
        处理文件并返回处理后的文件路径和生成的文件名
        实际处理逻辑可根据需求进行修改
        """
        # 生成处理后的文件名
        processed_filename = f"processed_{filename}"
        processed_path = os.path.join(self.upload_dir, processed_filename)
        
        # 在这里实现文件处理逻辑
        # 示例：简单复制文件作为"处理"
        with open(file_path, "rb") as source:
            with open(processed_path, "wb") as target:
                target.write(source.read())
        
        return processed_path, processed_filename
    
    def get_file_path(self, filename: str) -> str:
        """获取文件完整路径"""
        return os.path.join(self.upload_dir, filename) 