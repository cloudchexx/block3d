"""Text-based User Interface (TUI) for remote server use. Requires: pip install textual"""

import os
import sys

try:
    from .core import DataGenerator
except ImportError:
    from core import DataGenerator


def main():
    try:
        import textual
    except ImportError:
        print("TUI 模式需要 textual 库。请运行: pip install textual")
        sys.exit(1)

    from textual.app import App, ComposeResult
    from textual.containers import Container, Horizontal, Vertical
    from textual.widgets import (
        Button, Header, Footer, Static, Input, Label,
        ProgressBar, RadioSet, RadioButton,
    )
    from textual import work

    class GeneratorTUI(App):
        TITLE = "三维浮点数据生成器 v1.0"
        SUB_TITLE = "TUI模式 - 适用于远端服务器"
        CSS = """
        Screen { background: $surface; }
        Container { margin: 1 2; }
        .section {
            padding: 1 2; margin-bottom: 1;
            border: solid $primary;
            background: $surface-darken-1;
        }
        .section-title { text-style: bold; color: $accent; padding-bottom: 1; }
        .row { height: 3; align: center middle; }
        .dim-input { width: 10; }
        .dim-label { width: 14; }
        .range-input { width: 16; }
        .range-label { width: 8; }
        #output-path { width: 1fr; }
        ProgressBar { width: 1fr; height: 1; }
        #status-text { height: 1; color: $text-muted; }
        #estimate-text { height: 1; color: $text-muted; }
        Button { margin: 0 1; }
        #btn-generate { background: $success; }
        #btn-cancel { background: $error; }
        """

        def __init__(self):
            super().__init__()
            self._generator: DataGenerator | None = None

        def compose(self) -> ComposeResult:
            yield Header()
            yield Container(
                Vertical(
                    Label("数据维度设置", classes="section-title"),
                    Horizontal(
                        Label("X维度大小:", classes="dim-label"),
                        Input(value="100", id="dim-x", classes="dim-input"),
                        Label("Y维度大小:", classes="dim-label"),
                        Input(value="100", id="dim-y", classes="dim-input"),
                        Label("Z维度大小:", classes="dim-label"),
                        Input(value="100", id="dim-z", classes="dim-input"),
                        classes="row",
                    ),
                    classes="section",
                ),
                Vertical(
                    Label("数值范围设置", classes="section-title"),
                    Horizontal(
                        Label("最小值:", classes="range-label"),
                        Input(value="0.0", id="min-val", classes="range-input"),
                        Label("最大值:", classes="range-label"),
                        Input(value="1.0", id="max-val", classes="range-input"),
                        classes="row",
                    ),
                    classes="section",
                ),
                Vertical(
                    Label("生成引擎", classes="section-title"),
                    Horizontal(
                        RadioSet(
                            RadioButton("numpy (快速)", value=True, id="eng-numpy"),
                            RadioButton("纯Python", id="eng-python"),
                            id="engine-select",
                        ),
                        classes="row",
                    ),
                    classes="section",
                ),
                Vertical(
                    Label("输出设置", classes="section-title"),
                    Horizontal(
                        Label("输出路径:"),
                        Input(
                            value=os.path.join(os.getcwd(), "test.dat"),
                            id="output-path",
                        ),
                        classes="row",
                    ),
                    classes="section",
                ),
                Vertical(
                    Label("生成进度", classes="section-title"),
                    ProgressBar(total=100, id="progress-bar"),
                    Label("就绪", id="status-text"),
                    Label("", id="estimate-text"),
                    classes="section",
                ),
                Horizontal(
                    Button("开始生成", id="btn-generate", variant="success"),
                    Button("取消", id="btn-cancel", variant="error", disabled=True),
                    id="button-row",
                ),
            )
            yield Footer()

        def on_mount(self):
            has_np = DataGenerator.has_numpy()
            eng_set = self.query_one("#engine-select", RadioSet)
            eng_set._nodes[0].disabled = not has_np
            if not has_np:
                eng_set._nodes[1].value = True
            self._update_estimate()

        def on_input_changed(self, _event: Input.Changed):
            self._update_estimate()

        def on_radio_set_changed(self, _event: RadioSet.Changed):
            self._update_estimate()

        def _get_params(self):
            try:
                dx = int(self.query_one("#dim-x", Input).value)
                dy = int(self.query_one("#dim-y", Input).value)
                dz = int(self.query_one("#dim-z", Input).value)
            except ValueError:
                return None
            try:
                min_v = float(self.query_one("#min-val", Input).value)
                max_v = float(self.query_one("#max-val", Input).value)
            except ValueError:
                return None
            out = self.query_one("#output-path", Input).value.strip()
            use_np = self.query_one("#eng-numpy", RadioButton).value
            return dx, dy, dz, min_v, max_v, out, use_np

        def _update_estimate(self):
            params = self._get_params()
            est = self.query_one("#estimate-text", Label)
            if params is None:
                est.update("输入数值不合法，请检查")
                return
            dx, dy, dz, _min_v, _max_v, _out, use_np = params
            pts = dx * dy * dz
            header_size = 4 + 4 + 8 * 3 + 4 * 2 + 8
            size = pts * 4 + header_size
            eng = "numpy" if use_np else "python"
            est.update(
                f"引擎: {eng}  |  预估数据点数: {DataGenerator.format_number(pts)}  |  "
                f"预估文件大小: {DataGenerator.format_size(size)}"
            )

        def _set_controls_enabled(self, enabled: bool):
            self.query_one("#btn-generate", Button).disabled = not enabled
            self.query_one("#btn-cancel", Button).disabled = enabled
            for did in ("#dim-x", "#dim-y", "#dim-z", "#min-val", "#max-val", "#output-path"):
                self.query_one(did, Input).disabled = not enabled
            self.query_one("#engine-select", RadioSet).disabled = not enabled

        def on_button_pressed(self, event: Button.Pressed):
            if event.button.id == "btn-generate":
                self._start_generation()
            elif event.button.id == "btn-cancel":
                if self._generator is not None:
                    self._generator.stop()
                    self.query_one("#status-text", Label).update("正在取消...")

        @work(thread=True)
        def _start_generation(self):
            params = self._get_params()
            status_label = self.query_one("#status-text", Label)

            if params is None:
                self.call_from_thread(status_label.update, "参数错误: 输入数值不合法")
                return
            dx, dy, dz, min_v, max_v, out, use_np = params

            if min_v >= max_v:
                self.call_from_thread(status_label.update, "参数错误: 最小值必须小于最大值")
                return
            if out == "":
                self.call_from_thread(status_label.update, "参数错误: 请指定输出文件路径")
                return

            self.call_from_thread(self._set_controls_enabled, False)
            pb = self.query_one("#progress-bar", ProgressBar)
            self.call_from_thread(pb.update, progress=0)

            gen = DataGenerator(use_numpy=use_np)
            gen.set_dimensions(dx, dy, dz)
            gen.set_output_path(out)
            gen.set_value_range(min_v, max_v)
            self._generator = gen

            last_pct = [0]

            def on_progress(pct: int, msg: str):
                if pct != last_pct[0]:
                    last_pct[0] = pct
                    self.call_from_thread(pb.update, progress=pct)
                    self.call_from_thread(status_label.update, msg)

            success = gen.generate(progress_callback=on_progress)
            self._generator = None

            if not success:
                self.call_from_thread(status_label.update, "已取消")
            else:
                pts = DataGenerator.format_number(gen.total_data_points)
                sz = DataGenerator.format_size(gen.estimated_file_size)
                self.call_from_thread(status_label.update,
                                      f"完成! 文件大小: {sz}, 数据点数: {pts}")
            self.call_from_thread(self._set_controls_enabled, True)

    app = GeneratorTUI()
    app.run()


if __name__ == "__main__":
    main()
