"""Run-test-like public Python API example for block3d.

这个脚本只通过正式 Python 模块 `block3d` 工作，演示：
1. raw float32 .dat -> .b3d 转换；
2. 打开 .b3d 并读取 info；
3. 按 X/Y/Z 三轴生成 random/sequential 切片计划；
4. 用 Reader.read_slices() 批量读取；
5. 将每张切片落盘为无头 float32 .raw；
6. 输出读、写、总耗时。

它有意不实现 cold/hot cache scrub；这里的计时是当前 OS 页缓存状态下的普通 API
功能验收计时，不能和正式冷/热 benchmark 混用。
"""

from __future__ import annotations

import argparse
import hashlib
import random
import time
from pathlib import Path

import numpy as np
import block3d

# 作用：根据轴名获取维度大小
def axis_dim(info: dict, axis: str) -> int:
    if axis == "x":
        return int(info["dim_x"])
    if axis == "y":
        return int(info["dim_y"])
    if axis == "z":
        return int(info["dim_z"])
    raise ValueError(f"未知轴: {axis}")

# 作用：生成随机索引列表
def make_random_indices(dim: int, count: int, seed: int) -> list[int]:
    rng = random.Random(seed)
    return [rng.randrange(dim) for _ in range(count)]

# 作用：生成顺序索引列表
def make_sequential_indices(dim: int, count: int, start: int) -> list[int]:
    return list(range(start, min(dim, start + count)))

# 作用：生成计划的哈希值，用于唯一标识读取计划
def plan_hash(axis: str, mode: str, indices: list[int]) -> str:
    h = hashlib.blake2b(digest_size=8)
    h.update(axis.encode("utf-8"))
    h.update(mode.encode("utf-8"))
    for index in indices:
        h.update(int(index).to_bytes(8, "little", signed=False))
    return h.hexdigest()

# 作用：对数组进行轻量抽样计算 checksum，避免对大切片做全量扫描
def sampled_checksum(array: np.ndarray) -> float:
    """轻量抽样 checksum，避免对 test18/test50 大切片做额外全量扫描。"""
    flat = array.ravel()
    if flat.size == 0:
        return 0.0
    samples = min(64, flat.size)
    stride = max(1, flat.size // samples)
    picked = flat[::stride][:samples]
    return float(np.sum(picked, dtype=np.float64) + float(flat[-1]))

# 作用：生成输出路径，按 axis/mode/request_pos/index 命名
def output_path(output_dir: Path, axis: str, mode: str, request_pos: int, index: int) -> Path:
    return output_dir / axis / mode / f"{axis}_{mode}_{request_pos:04d}_{index}.raw"

# 作用：执行一个读取计划，读取切片并写入输出目录
def run_case(
    reader: block3d.Reader,
    axis: str,
    mode: str,
    indices: list[int],
    batch_window: int,
    output_dir: Path,
) -> dict:
    """读取一个 axis/mode 请求计划，并把每张切片写成无头 float32 .raw。"""
    read_sec = 0.0
    write_sec = 0.0
    output_bytes = 0
    checksum = 0.0
    slice_elems = 0

    case_start = time.perf_counter()
    for start in range(0, len(indices), batch_window):
        window = indices[start : start + batch_window]

        # 批量切片是 run_test 的核心读取路径；这里通过正式 Python API 返回 ndarray。
        t0 = time.perf_counter()
        batch = reader.read_slices(axis, window)
        t1 = time.perf_counter()
        read_sec += t1 - t0

        checksum += sampled_checksum(batch)
        slice_elems = int(batch[0].size) if len(window) else 0

        t2 = time.perf_counter()
        for offset, index in enumerate(window):
            out = output_path(output_dir, axis, mode, start + offset, index)
            out.parent.mkdir(parents=True, exist_ok=True)
            # tofile() 写无头 float32，与 run_test 的切片输出格式一致。
            batch[offset].astype(np.float32, copy=False).tofile(out)
            output_bytes += int(batch[offset].nbytes)
        t3 = time.perf_counter()
        write_sec += t3 - t2

    total_sec = time.perf_counter() - case_start
    slices = len(indices)
    return {
        "axis": axis,
        "mode": mode,
        "slices": slices,
        "slice_elems": slice_elems,
        "output_bytes": output_bytes,
        "read_sec": read_sec,
        "write_sec": write_sec,
        "total_sec": total_sec,
        "read_ms_per_slice": read_sec * 1000.0 / slices if slices else 0.0,
        "write_ms_per_slice": write_sec * 1000.0 / slices if slices else 0.0,
        "total_ms_per_slice": total_sec * 1000.0 / slices if slices else 0.0,
        "checksum": checksum,
        "plan_hash": plan_hash(axis, mode, indices),
    }

def mode_cn(mode: str) -> str:
    if mode == "random":
        return "随机"
    if mode == "sequential":
        return "顺序"
    return mode

# 作用：打印单个轴、单个模式的成绩，包括总耗时、平均每片毫秒和吞吐量
def print_result(result: dict) -> None:
    output_mib = result["output_bytes"] / 1024 / 1024
    throughput = output_mib / result["total_sec"] if result["total_sec"] > 0 else 0.0
    print(
        "API单项成绩 "
        f"轴={result['axis']} 模式={mode_cn(result['mode'])} "
        f"切片数={result['slices']} 每切片元素数={result['slice_elems']} "
        f"输出MiB={output_mib:.3f} 读取总耗时={result['read_sec']:.6f}秒 "
        f"写盘总耗时={result['write_sec']:.6f}秒 总耗时={result['total_sec']:.6f}秒 "
        f"平均读取={result['read_ms_per_slice']:.3f}毫秒/片 "
        f"平均写盘={result['write_ms_per_slice']:.3f}毫秒/片 "
        f"平均总耗时={result['total_ms_per_slice']:.3f}毫秒/片 "
        f"吞吐量={throughput:.3f}MiB/秒 校验和={result['checksum']:.6f} "
        f"计划哈希={result['plan_hash']}"
    )

# 作用：像 run_test 汇总一样，按随机/顺序汇总三轴平均成绩
def print_score_summary(results: list[dict]) -> None:
    print("\nAPI成绩汇总 单位=毫秒/片；平均总耗时包含读取、写文件和 Python 循环开销")
    for mode in ("random", "sequential"):
        mode_results = [r for r in results if r["mode"] == mode]
        if not mode_results:
            continue
        by_axis = {r["axis"]: r for r in mode_results}
        axis_parts = []
        for axis in ("x", "y", "z"):
            r = by_axis.get(axis)
            if r is None:
                continue
            axis_parts.append(
                f"{axis}轴:平均读取={r['read_ms_per_slice']:.3f},"
                f"平均写盘={r['write_ms_per_slice']:.3f},"
                f"平均总耗时={r['total_ms_per_slice']:.3f}"
            )
        avg_read = sum(r["read_ms_per_slice"] for r in mode_results) / len(mode_results)
        avg_write = sum(r["write_ms_per_slice"] for r in mode_results) / len(mode_results)
        avg_total = sum(r["total_ms_per_slice"] for r in mode_results) / len(mode_results)
        total_mib = sum(r["output_bytes"] for r in mode_results) / 1024 / 1024
        total_sec = sum(r["total_sec"] for r in mode_results)
        throughput = total_mib / total_sec if total_sec > 0 else 0.0
        print(
            "API模式汇总 "
            f"模式={mode_cn(mode)} 三轴成绩=[{'；'.join(axis_parts)}] "
            f"三轴平均读取={avg_read:.3f}毫秒/片 "
            f"三轴平均写盘={avg_write:.3f}毫秒/片 "
            f"三轴平均总耗时={avg_total:.3f}毫秒/片 "
            f"总体吞吐量={throughput:.3f}MiB/秒"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="转换原始数据，然后输出三轴 random/sequential API 读写成绩")
    parser.add_argument("--raw", required=True, help="输入无头 float32 原始 .dat 文件")
    parser.add_argument("--b3d", required=True, help="输出 .b3d 路径；转换时会覆盖")
    parser.add_argument("--dim-x", type=int, required=True)
    parser.add_argument("--dim-y", type=int, required=True)
    parser.add_argument("--dim-z", type=int, required=True)
    parser.add_argument("--output-dir", default="api_example_output_py", help="输出 .raw 切片目录")
    parser.add_argument("--layout", default="legacy", choices=("legacy", "micro-tiled"))
    parser.add_argument("--block-size", type=int, default=0, help="0 = 自动")
    parser.add_argument("--micro-size", type=int, default=0)
    parser.add_argument("--threads", type=int, default=0, help="0 = 自动")
    parser.add_argument("--memory-limit", type=int, default=0, help="软内存限制（MiB）")
    parser.add_argument("--random-count", type=int, default=100)
    parser.add_argument("--seq-count", type=int, default=10)
    parser.add_argument("--batch-window", type=int, default=1, help="每次 read_slices() 调用的切片数")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--seq-start", type=int, default=0)
    parser.add_argument("--verify-samples", type=int, default=0, help="可选的原始与 b3d 随机点验证")
    parser.add_argument("--progress", action="store_true", help="显示转换进度")
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    batch_window = max(1, args.batch_window)

    convert_t0 = time.perf_counter()
    # 作用：调用 block3d API 将 headerless float32 raw 数据转换为 .b3d 格式
    # 如果本身转换就存在，直接验证info信息，不对在转换，对了就跳过
    if Path(args.b3d).exists():
        reader = block3d.Reader(args.b3d, threads=args.threads, memory_limit_mb=args.memory_limit, read_dispatch="round-robin")
        info = reader.info()
        if (
            info["dim_x"] != args.dim_x
            or info["dim_y"] != args.dim_y
            or info["dim_z"] != args.dim_z
            or info["layout"] != args.layout
            or info["block_size"] != args.block_size
            or info["micro_size"] != args.micro_size
        ):
            print(
                f"现有 b3d 文件 {args.b3d} 与指定参数不匹配，重新转换。"
                f"现有维度={info['dim_x']}x{info['dim_y']}x{info['dim_z']} "
                f"块大小={info['block_size']} 布局={info['layout']} 微块大小={info['micro_size']}"
            )
        else:
            print(f"使用现有 b3d 文件 {args.b3d}")
            convert_sec = 0.0
    else:
        block3d.convert_raw_to_b3d(
            raw_path=args.raw,
            b3d_path=args.b3d,
            dim_x=args.dim_x,
            dim_y=args.dim_y,
            dim_z=args.dim_z,
            block_size=args.block_size,
            layout=args.layout,
            threads=args.threads,
            memory_limit_mb=args.memory_limit,
            micro_size=args.micro_size,
            progress=args.progress,
        )
        convert_sec = time.perf_counter() - convert_t0

    # 作用：创建 block3d.Reader 对象，读取 .b3d 文件信息
    reader = block3d.Reader(args.b3d, threads=args.threads, memory_limit_mb=args.memory_limit, read_dispatch="round-robin")
    info = reader.info()
    # 作用：打印转换结果和读取信息，包括维度、块大小、格式版本、布局、微块大小和转换耗时
    print(
        "API转换结果 "
        f"原始文件={args.raw} b3d文件={args.b3d} "
        f"维度={info['dim_x']}x{info['dim_y']}x{info['dim_z']} "
        f"块大小={info['block_size']} 格式版本={info['format_version']} "
        f"布局={info['layout']} 微块大小={info['micro_size']} "
        f"转换耗时={convert_sec:.6f}秒"
    )

    if args.verify_samples > 0:
        verify_t0 = time.perf_counter()
        ok = reader.verify(args.raw, samples=args.verify_samples, tolerance=1e-3)
        verify_sec = time.perf_counter() - verify_t0
        print(f"API验证结果 是否通过={int(ok)} 采样数={args.verify_samples} 验证耗时={verify_sec:.6f}秒")
        if not ok:
            return 3

    # 作用：对 X/Y/Z 三轴分别生成随机和顺序切片计划，并执行读取和写入操作。
    # mode 保持英文，方便日志后处理；终端输出中会按 ms/切片汇总成绩。
    results = []
    for axis in ("x", "y", "z"):
        dim = axis_dim(info, axis)
        random_indices = make_random_indices(dim, args.random_count, args.seed + ord(axis))
        random_result = run_case(reader, axis, "random", random_indices, batch_window, output_dir)
        print_result(random_result)
        results.append(random_result)

        sequential_indices = make_sequential_indices(dim, args.seq_count, args.seq_start)
        sequential_result = run_case(reader, axis, "sequential", sequential_indices, batch_window, output_dir)
        print_result(sequential_result)
        results.append(sequential_result)

    print_score_summary(results)

    # 额外演示项目需求中的 X 主维单列、子体积和单点 API。
    xcol = reader.read_column("x", 0, 0)
    sub = reader.read_subvolume(0, min(4, int(info["dim_x"])), 0, min(4, int(info["dim_y"])), 0, min(4, int(info["dim_z"])))
    print(
        "API额外结果 "
        f"X列长度={xcol.shape[0]} X列首值={float(xcol[0])} X列尾值={float(xcol[-1])} "
        f"子体积形状={sub.shape[0]}x{sub.shape[1]}x{sub.shape[2]} 点(0,0,0)值={float(reader.read_point(0, 0, 0))}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())