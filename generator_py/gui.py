import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import threading
import os

try:
    from .core import DataGenerator
except ImportError:
    from core import DataGenerator


class GeneratorGUI(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("三维浮点数据生成器 v1.0")
        self.resizable(False, False)
        self.configure(bg="#f5f7fa")

        self._generator = DataGenerator()
        self._work_thread: threading.Thread | None = None
        self._running = False
        self._use_numpy = tk.BooleanVar(value=DataGenerator.has_numpy())
        self._setup_ui()
        self._update_estimate()

        self.update_idletasks()
        w = self.winfo_reqwidth()
        h = self.winfo_reqheight()
        ws = self.winfo_screenwidth()
        hs = self.winfo_screenheight()
        x = (ws - w) // 2
        y = (hs - h) // 2
        self.geometry(f"+{x}+{y}")

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _setup_ui(self):
        pad = {"padx": 20, "pady": 5}
        frame = ttk.Frame(self, padding=20)
        frame.pack(fill=tk.BOTH, expand=True)

        # Title
        ttk.Label(
            frame, text="三维浮点数据生成器",
            font=("Microsoft YaHei", 16, "bold"),
            foreground="#2c3e50", background="#f5f7fa"
        ).pack(pady=(0, 10))

        # ---- 维度设置 ----
        dim_frame = ttk.LabelFrame(frame, text="数据维度设置", padding=10)
        dim_frame.pack(fill=tk.X, **pad)

        self._dim_x_var = tk.IntVar(value=100)
        self._dim_y_var = tk.IntVar(value=100)
        self._dim_z_var = tk.IntVar(value=100)
        for v in (self._dim_x_var, self._dim_y_var, self._dim_z_var):
            v.trace("w", lambda *a: self._update_estimate())

        self._make_labeled_spin(dim_frame, "X维度大小:", 1, 100000, self._dim_x_var, 0)
        self._make_labeled_spin(dim_frame, "Y维度大小:", 1, 100000, self._dim_y_var, 1)
        self._make_labeled_spin(dim_frame, "Z维度大小:", 1, 100000, self._dim_z_var, 2)

        # ---- 数值范围 ----
        range_frame = ttk.LabelFrame(frame, text="数值范围设置", padding=10)
        range_frame.pack(fill=tk.X, **pad)

        ttk.Label(range_frame, text="最小值:").grid(row=0, column=0, sticky=tk.W)
        self._min_val = tk.DoubleVar(value=0.0)
        ttk.Spinbox(
            range_frame, from_=-1e9, to=1e9, textvariable=self._min_val,
            width=15, format="%.6f"
        ).grid(row=0, column=1, padx=(5, 20))

        ttk.Label(range_frame, text="最大值:").grid(row=0, column=2, sticky=tk.W)
        self._max_val = tk.DoubleVar(value=1.0)
        ttk.Spinbox(
            range_frame, from_=-1e9, to=1e9, textvariable=self._max_val,
            width=15, format="%.6f"
        ).grid(row=0, column=3, padx=(5, 0))

        # ---- 引擎选择 ----
        if DataGenerator.has_numpy():
            eng_frame = ttk.LabelFrame(frame, text="生成引擎", padding=5)
            eng_frame.pack(fill=tk.X, **pad)
            ttk.Radiobutton(
                eng_frame, text="numpy (快速)", variable=self._use_numpy, value=True
            ).pack(side=tk.LEFT, padx=(0, 15))
            ttk.Radiobutton(
                eng_frame, text="纯Python", variable=self._use_numpy, value=False
            ).pack(side=tk.LEFT)

        # ---- 输出路径 ----
        out_frame = ttk.LabelFrame(frame, text="输出设置", padding=10)
        out_frame.pack(fill=tk.X, **pad)

        ttk.Label(out_frame, text="输出路径:").pack(side=tk.LEFT)
        default_path = os.path.join(os.getcwd(), "test.dat")
        self._out_path = tk.StringVar(value=default_path)
        ttk.Entry(
            out_frame, textvariable=self._out_path, state="readonly", width=50
        ).pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(5, 5))
        ttk.Button(out_frame, text="浏览...", command=self._browse).pack(side=tk.LEFT)

        # ---- 进度 ----
        prog_frame = ttk.LabelFrame(frame, text="生成进度", padding=10)
        prog_frame.pack(fill=tk.X, **pad)

        self._progress = ttk.Progressbar(
            prog_frame, orient=tk.HORIZONTAL, length=460, mode="determinate"
        )
        self._progress.pack(fill=tk.X, pady=(0, 5))

        self._status_label = ttk.Label(prog_frame, text="就绪", foreground="#909399")
        self._status_label.pack(fill=tk.X)

        self._estimate_label = ttk.Label(prog_frame, text="", foreground="#909399")
        self._estimate_label.pack(fill=tk.X)

        # ---- 按钮 ----
        btn_frame = ttk.Frame(frame)
        btn_frame.pack(fill=tk.X, pady=(10, 0))
        btn_frame.columnconfigure(0, weight=1)
        btn_frame.columnconfigure(2, weight=1)

        self._btn_generate = ttk.Button(
            btn_frame, text="开始生成", command=self._start
        )
        self._btn_generate.grid(row=0, column=1, padx=10)

        self._btn_cancel = ttk.Button(
            btn_frame, text="取消", command=self._cancel, state=tk.DISABLED
        )
        self._btn_cancel.grid(row=0, column=2, padx=10, sticky=tk.W)

    def _make_labeled_spin(self, parent, label, lo, hi, var, row):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky=tk.W, pady=3)
        ttk.Spinbox(
            parent, from_=lo, to=hi, textvariable=var, width=12
        ).grid(row=row, column=1, sticky=tk.W, pady=3, padx=(5, 0))

    def _browse(self):
        path = filedialog.asksaveasfilename(
            title="选择输出文件",
            defaultextension=".dat",
            filetypes=[("二进制文件", "*.bin *.dat"), ("所有文件", "*.*")],
            initialdir=os.path.dirname(self._out_path.get()) or os.getcwd(),
        )
        if path:
            self._out_path.set(path)

    def _update_estimate(self):
        try:
            dx = self._dim_x_var.get()
            dy = self._dim_y_var.get()
            dz = self._dim_z_var.get()
        except (tk.TclError, ValueError):
            return
        self._generator.set_dimensions(dx, dy, dz)
        pts = self._generator.total_data_points
        size = self._generator.estimated_file_size
        eng = "numpy" if (DataGenerator.has_numpy() and self._use_numpy.get()) else "python"
        self._estimate_label.config(
            text=f"引擎: {eng}  |  预估数据点数: {DataGenerator.format_number(pts)}  |  "
                 f"预估文件大小: {DataGenerator.format_size(size)}"
        )

    def _set_controls_state(self, enabled: bool):
        gen_state = tk.NORMAL if enabled else tk.DISABLED
        self._btn_generate.config(state=gen_state)
        self._btn_cancel.config(state=tk.DISABLED if enabled else tk.NORMAL)

    def _start(self):
        min_v = self._min_val.get()
        max_v = self._max_val.get()
        if min_v >= max_v:
            messagebox.showwarning("参数错误", "最小值必须小于最大值！")
            return

        out = self._out_path.get().strip()
        if not out:
            messagebox.showwarning("参数错误", "请指定输出文件路径！")
            return

        self._generator = DataGenerator(use_numpy=self._use_numpy.get())
        self._generator.set_dimensions(
            self._dim_x_var.get(), self._dim_y_var.get(), self._dim_z_var.get()
        )
        self._generator.set_output_path(out)
        self._generator.set_value_range(min_v, max_v)

        self._set_controls_state(False)
        self._progress["value"] = 0
        self._status_label.config(text="正在生成数据...")
        self._running = True

        self._work_thread = threading.Thread(target=self._run_generation, daemon=True)
        self._work_thread.start()

    def _run_generation(self):
        success = self._generator.generate(progress_callback=self._on_progress)
        self.after(0, self._on_finished, success)

    def _on_progress(self, pct: int, msg: str):
        def update():
            self._progress["value"] = pct
            self._status_label.config(text=msg)
        self.after(0, update)

    def _on_finished(self, success: bool):
        self._running = False
        self._set_controls_state(True)
        pts = DataGenerator.format_number(self._generator.total_data_points)
        size = DataGenerator.format_size(self._generator.estimated_file_size)

        if success:
            self._progress["value"] = 100
            msg = f"数据生成完成！\n文件大小：{size}\n数据点数：{pts}"
            self._status_label.config(text="完成")
            messagebox.showinfo("生成完成", msg)
        elif self._generator._stop_flag.is_set():
            self._status_label.config(text="已取消")
            messagebox.showwarning("生成取消", "用户取消操作")
        else:
            self._status_label.config(text="失败")

    def _cancel(self):
        self._generator.stop()
        self._status_label.config(text="正在取消...")

    def _on_close(self):
        if self._running:
            self._generator.stop()
        self.destroy()
