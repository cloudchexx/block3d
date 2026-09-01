# Block3D

面向多维度高效访问的三维数据存储结构与高速读写算法库。

Block3D 是第十三届东方杯全国大学生勘探地球物理大赛软件开发赛道的参赛作品。它针对大规模规则三维数据体（如三维叠后地震数据）在多视角切片、剖面浏览、局部精细查询等高频操作下的多点随机 IO 问题，设计了一种**无冗余、高均衡、高性能**的三维数据组织方式：通过物理布局优化、切片预对齐与快速定位索引，在不增加冗余存储的前提下，使 X、Y、Z 三个方向均具备接近连续读取的高效读写能力。

- 语言 / 构建：C++17、C（C API）、CMake（>= 3.14），可选 pybind11 Python 绑定。
- 平台：Windows（MSVC，含 OpenMP）、Linux / macOS（GCC / Clang）。
- 版本：1.0.0

---

## 背景与目标

规则三维数据体在磁盘上普遍采用单一维度顺序存储（如沿 Z 连续排布）。这种组织方式沿存储维度访问效率较高，但在沿 X、Y 两个维度做剖面读取时会产生大量磁盘随机 IO 与离散寻址，导致加载卡顿、交互响应迟缓，在 TB 级数据规模下尤其明显。

传统方案用多副本冗余存储（三个维度各生成一份顺序文件）来缓解，但存储占用成倍增长，成本与同步负担巨大。Block3D 的目标是：**无需多副本，即可让 X、Y、Z 三个维度的访问效率均衡且接近连续读取的极限**，在存储效率与 IO 性能之间取得最优平衡。

具体包括：

- 原始浮点数组与优化存储格式（`.b3d`）的双向转换；
- 沿 X、Y、Z 任意维度的单张切片 / 批量切片读取；
- 按主维度的单列（column）高效读取；
- 子体积（subvolume）与单点读取；
- 单进程多线程并行，内存使用量可配置、可管控；
- 数据读写结果精确还原原始三维数据，单点相对误差小于千分之一。

---

## 核心特性

- **无冗余、空间紧凑**：存储结构不含多副本，空间占用远小于原始数据的 1.5 倍，满足大赛存储指标。
- **三轴均衡访问**：X / Y / Z 三方向切片读取效率接近，支持随机切片（100 次随机坐标）与连续切片（10 次连续大切片）。
- **优化的存储结构**：v2 micro-tiled 布局（micro_size=8、block_size=128），通过物理重排与切片预对齐降低随机 IO。
- **可配置内存上限**：通过 `--memory-limit / max_memory_mb` 参数设置最大可用内存，避免过度占用系统内存。
- **多线程并行**：单进程多线程（OpenMP），支持并行转换、并行切片读取与写盘。
- **双语言 API**：C API（`block3d/block3d.h`）与 C++ API（`block3d/block3d.hpp`），并有 pybind11 Python 绑定。
- **可校验**：支持随机点 verify（默认容差 `1e-3`），用于确认数据还原正确。

---

## 目录结构

```text
block3d-cpp/
  CMakeLists.txt                顶层 CMake 工程
  cmake/                        CMake package config 与安装规则
  include/block3d/              公共头文件
    block3d.h                   C API
    block3d.hpp                 C++ API（Reader、Array、SliceBatch 等）
    core.hpp / reader.hpp / types.hpp / rng.hpp / converter.hpp  内部接口
    version.h                   版本宏
  src/
    lib/                        核心实现：block3d_c_api / block3d_cpp_api / core / reader / converter
    cli/                        block3d_cli 命令行工具
    benchmark/                  run_test / benchmark_cache 内部基准工具
  tests/                        C/C++ 测试与测试固件（tests/fixtures/*.log）
  examples/                     C / C++ / Python / consumer_cmake 示例
  python/                       pybind11 Python 绑定（pyproject.toml）
  generator_py/                 三维测试数据生成器（用于生成无头 float32 .dat）
  tools/                        辅助脚本（compare_dispatch_ab.py 等）
  算法API使用手册.md              C/C++/Python API 使用说明
  项目需求文档.md                赛题说明与需求
```

---

## 构建需求

- CMake >= 3.14
- 支持 C++17 的编译器：
  - Windows：MSVC（Visual Studio 2022 Build Tools）或 MinGW，OpenMP enabled
  - Linux / macOS：GCC 或 Clang
- 可选：Python3（构建 Python 模块）、pybind11、NumPy

```bash
# 配置
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 构建
cmake --build build --config Release

# 运行测试
ctest --test-dir build -C Release --output-on-failure
```

### CMake 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `BLOCK3D_BUILD_SHARED` | OFF | 构建为共享库 |
| `BLOCK3D_BUILD_CLI` | ON | 构建 `block3d_cli` |
| `BLOCK3D_BUILD_RUN_TEST` | ON | 构建 `run_test` 基准工具 |
| `BLOCK3D_BUILD_TESTS` | ON | 构建 C/C++ 测试 |
| `BLOCK3D_BUILD_EXAMPLES` | ON | 构建示例 |
| `BLOCK3D_BUILD_PYTHON` | OFF | 构建 Python 模块 |
| `BLOCK3D_INSTALL` | ON | 启用安装规则 |

### 安装

```bash
cmake --install build --config Release --prefix <prefix>
```

安装后可通过 `find_package(block3d CONFIG REQUIRED)` 在外部 CMake 项目中使用：

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_app LANGUAGES CXX)

find_package(block3d CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE block3d::block3d)
```

---

## C/C++ API 概述

`include/block3d/block3d.hpp` 主要接口：

```cpp
namespace block3d {

// 版本号
Version version();

// 无头 float32 raw（X-Y-Z 顺序，Z 连续）转换为 .b3d
void convert_raw_to_b3d(
    const std::string& raw_path,
    const std::string& b3d_path,
    std::uint64_t dim_x, std::uint64_t dim_y, std::uint64_t dim_z,
    const ConvertOptionsFacade& options = {});

class Reader {
public:
    explicit Reader(const std::string& b3d_path,
                    const ReaderOptionsFacade& options = {});

    FileInfo info() const;                                        // 维度、block size、layout、micro size 等

    Array      read_slice(Axis axis, std::uint64_t index) const;  // 单张切片
    SliceBatch read_slices(Axis axis, const std::vector<std::uint64_t>& indices) const;  // 批量切片
    Array      read_column(Axis axis, std::uint64_t coord1, std::uint64_t coord2) const; // 单列
    Array      read_subvolume(std::uint64_t xs, std::uint64_t xe,
                              std::uint64_t ys, std::uint64_t ye,
                              std::uint64_t zs, std::uint64_t ze) const;                // 子体积
    float      read_point(std::uint64_t x, std::uint64_t y, std::uint64_t z) const;     // 单点

    bool verify(const std::string& raw_path,
                std::uint64_t samples = 1000,
                float tolerance = 1e-3f) const;                  // 抽查校验
};

} // namespace block3d
```

`Axis` 枚举：`X | Y | Z`。`Layout` 枚举：`LegacyXYZ`（v1 legacy）与 `MicroTiledXYZ`（v2 micro-tiled）。`ReadDispatch` 枚举：`RoundRobin | Contiguous`。

C API 见 `include/block3d/block3d.h`，接口与 C++ 一一对应（如 `block3d_reader_create`、`block3d_reader_read_slice` 等）。

---

## Python 模块

源码中包含 pybind11 Python 绑定，包名为 `block3d`，依赖 `numpy`，支持 Python >= 3.8。

```bash
# 使用 scikit-build 安装
pip install ./python

# 或按 CMake 方式构建
cmake -S . -B build_python \
  -DBLOCK3D_BUILD_PYTHON=ON \
  -DBLOCK3D_BUILD_TESTS=OFF -DBLOCK3D_BUILD_CLI=OFF \
  -DBLOCK3D_BUILD_RUN_TEST=OFF -DBLOCK3D_BUILD_EXAMPLES=OFF \
  -DPython3_EXECUTABLE=$(python -c "import sys; print(sys.executable)")
cmake --build build_python --config Release --target block3d_python
```

> 不同机器的 Python / NumPy / pybind11 路径差异较大，Python `.pyd` 一般建议在目标机器上现场构建。

---

## CLI 工具

`block3d_cli` 提供转换、信息查看、校验、切片导出等能力：

```bash
# 查看 .b3d 文件信息
block3d_cli info out.b3d

# 将无头 float32 raw 转换为 v2 micro-tiled .b3d
block3d_cli convert in.dat out.b3d \
  --dim-x 801 --dim-y 2405 --dim-z 2501 \
  --block-size 128 --threads 16 \
  --layout micro-tiled --micro-size 8

# 校验 .b3d 与原始 .dat 的一致性
block3d_cli verify out.b3d in.dat --samples 1000

# 导出 X 方向第 0 张切片为无头 float32 .raw
block3d_cli extract out.b3d --axis x --index 0 -o x0.raw
```

---

## 示例

- `examples/c/basic_read_slice.c`：C 语言基础切片读取。
- `examples/cpp/basic_read_slice.cpp`：C++ 基础切片读取。
- `examples/cpp/api_random_sequential.cpp`：正式 C++ API 测试（随机 / 连续切片，三轴读、写、吞吐量）。
- `examples/python/api_random_sequential.py`：正式 Python API 测试。
- `examples/consumer_cmake/`：外部 CMake 消费示例。

`api_random_sequential` 会用无头 `float32 .dat` 转换出 `.b3d`，然后对 X/Y/Z 三轴分别执行 random 与 sequential 切片读取并写出 `.raw`，输出每轴每模式的耗时、吞吐量、校验和与计划哈希。

---

## 数据格式说明

- 输入 raw：无头 `float32`，按 **X-Y-Z** 顺序存储，Z 连续：

  ```text
  raw_index = x * dim_y * dim_z + y * dim_z + z
  ```

- 输出的 `.b3d` 为优化存储格式，v2 为 **micro-tiled** 布局（`micro_size=8`）。推荐显式使用 `--layout micro-tiled --micro-size 8 --block-size 128`。
- 可用 `generator_py/` 生成兼容的无头 `float32 .dat` 测试数据：

  ```bash
  python generator_py/run.py --cli \
    -x 801 -y 2405 -z 2501 \
    --min 0 --max 1 --engine numpy --no-header \
    -o test18.dat
  ```

---

## 文档

- [算法API使用手册.md](./算法API使用手册.md)：C/C++/Python API 调用方式与示例脚本字段说明。
- [项目需求文档.md](./项目需求文档.md)：赛题说明、已知条件、需求与评分标准。

---

## 致谢 / 说明

本作品为第十三届东方杯全国大学生勘探地球物理大赛软件开发赛道参赛作品，数据仅用于大赛开发与测试。
