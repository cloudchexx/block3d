import argparse
import os
import sys
import time

try:
    from .core import DataGenerator
except ImportError:
    from core import DataGenerator


def main():
    parser = argparse.ArgumentParser(
        description="三维浮点数据生成器 - 随机生成三维数据集"
    )
    parser.add_argument(
        "-x", "--dim-x", type=int, default=100,
        help="X维度大小 (默认: 100)"
    )
    parser.add_argument(
        "-y", "--dim-y", type=int, default=100,
        help="Y维度大小 (默认: 100)"
    )
    parser.add_argument(
        "-z", "--dim-z", type=int, default=100,
        help="Z维度大小 (默认: 100)"
    )
    parser.add_argument(
        "--min", type=float, default=0.0,
        help="随机数最小值 (默认: 0.0)"
    )
    parser.add_argument(
        "--max", type=float, default=1.0,
        help="随机数最大值 (默认: 1.0)"
    )
    parser.add_argument(
        "-o", "--output", type=str, default=None,
        help="输出文件路径 (默认: ./test.dat)"
    )
    parser.add_argument(
        "--engine", type=str, default="auto",
        choices=["auto", "numpy", "python"],
        help="生成引擎: auto(自动选择numpy), numpy, python (默认: auto)"
    )
    parser.add_argument(
        "--list-engines", action="store_true",
        help="列出可用的生成引擎并退出"
    )
    parser.add_argument(
        "--no-header", action="store_true",
        help="不写入文件头（仅原始float数据）"
    )

    args = parser.parse_args()

    if args.list_engines:
        engines = DataGenerator.engines()
        print(f"可用引擎: {', '.join(engines)}")
        print(f"numpy: {'已安装' if DataGenerator.has_numpy() else '未安装 (pip install numpy)'}")
        sys.exit(0)

    if args.dim_x <= 0 or args.dim_y <= 0 or args.dim_z <= 0:
        print("错误: 维度大小必须大于0")
        sys.exit(1)

    if args.min >= args.max:
        print("错误: 最小值必须小于最大值")
        sys.exit(1)

    output = args.output or os.path.join(os.getcwd(), "test.dat")

    use_numpy = None if args.engine == "auto" else (args.engine == "numpy")
    gen = DataGenerator(use_numpy=use_numpy)
    gen.set_dimensions(args.dim_x, args.dim_y, args.dim_z)
    gen.set_output_path(output)
    gen.set_value_range(args.min, args.max)

    total = gen.total_data_points
    est_size = gen.estimated_file_size
    print(f"数据维度: {args.dim_x} × {args.dim_y} × {args.dim_z}")
    print(f"总数据点: {DataGenerator.format_number(total)}")
    print(f"预估文件大小: {DataGenerator.format_size(est_size)}")
    print(f"引擎: {gen.engine}")
    print(f"输出路径: {output}")
    print()

    if args.no_header:
        gen._write_header = lambda f: None

    last_pct = [-1]

    def progress(pct: int, msg: str):
        if pct != last_pct[0]:
            last_pct[0] = pct
            bar = "#" * (pct // 2) + "-" * (50 - pct // 2)
            print(f"\r[{bar}] {pct:3d}%  {msg}", end="", file=sys.stderr)

    print("开始生成...")
    start_time = time.time()

    success = gen.generate(progress_callback=progress)

    elapsed = time.time() - start_time
    print(file=sys.stderr)

    if success:
        file_size = os.path.getsize(output)
        print(f"生成完成! 耗时 {elapsed:.2f}s, 速度 {file_size / elapsed / 1024 / 1024:.0f} MB/s, "
              f"文件大小 {DataGenerator.format_size(file_size)}")
    else:
        print("生成被取消或被中断")


if __name__ == "__main__":
    main()
