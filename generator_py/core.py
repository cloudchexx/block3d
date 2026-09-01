import struct
import time
import random
import os
import threading
from typing import Optional, Callable

try:
    import numpy as _np
    _HAS_NUMPY = True
except ImportError:
    _np = None
    _HAS_NUMPY = False


class DataGenerator:
    """三维浮点数据生成器，与C++版本保持二进制兼容。
    
    - numpy可用时自动使用numpy分块生成（速度接近磁盘I/O上限）
    - numpy不可用时退回纯Python实现
    """

    MAGIC = b"3DDF"
    VERSION = 1
    BUFFER_SIZE = 65536
    CHUNK_FLOATS = 32 * 1024 * 1024

    def __init__(self, use_numpy: Optional[bool] = None):
        self._dim_x = 100
        self._dim_y = 100
        self._dim_z = 100
        self._min_value = 0.0
        self._max_value = 1.0
        self._output_path = ""
        self._stop_flag = threading.Event()

        if use_numpy is None:
            use_numpy = _HAS_NUMPY
        self._use_numpy = bool(use_numpy and _HAS_NUMPY)

        if not self._use_numpy:
            self._rng = random.Random()

    @classmethod
    def has_numpy(cls) -> bool:
        return _HAS_NUMPY

    @classmethod
    def engines(cls):
        engines = []
        if _HAS_NUMPY:
            engines.append("numpy")
        engines.append("python")
        return engines

    @property
    def engine(self) -> str:
        return "numpy" if self._use_numpy else "python"

    def set_dimensions(self, dim_x: int, dim_y: int, dim_z: int):
        self._dim_x = dim_x
        self._dim_y = dim_y
        self._dim_z = dim_z

    def set_output_path(self, path: str):
        self._output_path = path

    def set_value_range(self, min_val: float, max_val: float):
        self._min_value = min_val
        self._max_value = max_val

    @property
    def total_data_points(self) -> int:
        return self._dim_x * self._dim_y * self._dim_z

    @property
    def estimated_file_size(self) -> int:
        header_size = 4 + 4 + 8 * 3 + 4 * 2 + 8
        return self.total_data_points * 4 + header_size

    def stop(self):
        self._stop_flag.set()

    def generate(
        self,
        progress_callback: Optional[Callable[[int, str], None]] = None
    ) -> bool:
        self._stop_flag.clear()

        if not self._use_numpy:
            self._rng.seed()

        if self._dim_x <= 0 or self._dim_y <= 0 or self._dim_z <= 0:
            if progress_callback:
                progress_callback(0, "维度大小不能为零")
            return False

        if not self._output_path:
            if progress_callback:
                progress_callback(0, "输出路径不能为空")
            return False

        out_dir = os.path.dirname(self._output_path)
        if out_dir and not os.path.exists(out_dir):
            os.makedirs(out_dir, exist_ok=True)

        engine_tag = f"[{self.engine}]"
        if progress_callback:
            progress_callback(0, f"正在准备生成数据... {engine_tag}")

        try:
            with open(self._output_path, "wb") as f:
                self._write_header(f)
                if self._use_numpy:
                    self._write_data_numpy(f, progress_callback)
                else:
                    self._write_data_python(f, progress_callback)
        except (IOError, OSError) as e:
            if progress_callback:
                progress_callback(0, f"文件写入失败: {e}")
            return False

        return not self._stop_flag.is_set()

    def _write_header(self, f):
        header = struct.pack(
            "<4sIQQQffQ",
            self.MAGIC,
            self.VERSION,
            self._dim_x,
            self._dim_y,
            self._dim_z,
            self._min_value,
            self._max_value,
            int(time.time()),
        )
        f.write(header)

    # ---- numpy 快速通道 ----

    def _write_data_numpy(self, f, progress_callback):
        total = self.total_data_points
        processed = 0
        last_pct = -1
        rng = _np.random.default_rng()

        while processed < total:
            if self._stop_flag.is_set():
                return

            chunk = min(self.CHUNK_FLOATS, total - processed)
            data = rng.uniform(
                self._min_value, self._max_value, size=chunk
            ).astype(_np.float32)
            f.write(data.tobytes())
            processed += chunk

            if progress_callback:
                pct = processed * 100 // total
                if pct != last_pct:
                    last_pct = pct
                    progress_callback(
                        pct,
                        f"正在生成数据 [numpy]... {pct}% "
                        f"({processed:,}/{total:,})",
                    )

    # ---- 纯 Python 回退通道 ----

    def _write_data_python(self, f, progress_callback):
        total = self.total_data_points
        buf = []
        processed = 0
        last_pct = -1

        for x in range(self._dim_x):
            if self._stop_flag.is_set():
                return
            for y in range(self._dim_y):
                for z in range(self._dim_z):
                    buf.append(self._rng.uniform(self._min_value, self._max_value))
                    processed += 1

                    if len(buf) >= self.BUFFER_SIZE:
                        f.write(struct.pack(f"<{len(buf)}f", *buf))
                        buf.clear()

                    if progress_callback:
                        pct = processed * 100 // total
                        if pct != last_pct:
                            last_pct = pct
                            progress_callback(
                                pct,
                                f"正在生成数据 [python]... {pct}% "
                                f"({processed:,}/{total:,})",
                            )

        if buf:
            f.write(struct.pack(f"<{len(buf)}f", *buf))

    @staticmethod
    def format_number(n: int) -> str:
        return f"{n:,}"

    @staticmethod
    def format_size(bytes_val: int) -> str:
        units = ["B", "KB", "MB", "GB", "TB"]
        size = float(bytes_val)
        idx = 0
        while size >= 1024.0 and idx < len(units) - 1:
            size /= 1024.0
            idx += 1
        return f"{size:.2f} {units[idx]}"
