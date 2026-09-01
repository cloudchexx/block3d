# 算法 API 使用手册

本文面向后续自行编写调用代码的用户，说明 `block3d` 打包后的 C / C++ / Python API 如何完成：

1. 原始无头 `float32 .dat` 转换为 `.b3d`；
2. 打开 `.b3d` 并读取元数据；
3. 随机 / 顺序读取 X/Y/Z 三轴切片；
4. 批量读取切片并落盘为无头 `float32 .raw`；
5. 读取 X 主维单列、子体积、单点；
6. 做 raw 与 b3d 的随机点正确性校验。

本文重点说明正式 API 使用，并把 `api_random_sequential` 的 C++ / Python 两个版本作为后期主要正式测试脚本。它们不做 cold/hot cache scrub，成绩代表当前系统缓存状态下的 API 读写验收计时；配置更严格的 `run_test.exe` 仅作为开发团队内部 cold/hot benchmark 工具。

---

## 1. 数据格式与坐标约定

### 1.1 原始 `.dat`

输入原始文件是无头 `float32` 数组，按 X-Y-Z 顺序存储，Z 连续：

```text
raw_index = x * dim_y * dim_z + y * dim_z + z
```

例如 `(x, y, z)` 对应的 float 在原始文件中的字节偏移为：

```text
byte_offset = (x * dim_y * dim_z + y * dim_z + z) * sizeof(float)
```

### 1.2 `.b3d`

`.b3d` 是本项目优化后的块式存储格式：

```text
64-byte FileHeader
uint64_t block_offsets[total_blocks]
padding to 4096-byte boundary
Morton/Z-order block data
```

当前支持：

| 格式 | 说明 |
|---|---|
| v1 legacy | 块内 legacy XYZ 布局，默认格式 |
| v2 micro-tiled | 块内 micro-tiled XYZ，目前 micro size 为 8 |

正式 API 会根据 `.b3d` header 自动识别 v1/v2，读取 API 的返回 shape 和布局不随磁盘内部布局变化。

### 1.3 坐标与范围

所有 API 均使用 0 基坐标：

```text
0 <= x < dim_x
0 <= y < dim_y
0 <= z < dim_z
```

子体积使用半开区间：

```text
[xs, xe) × [ys, ye) × [zs, ze)
```

必须满足：

```text
xs < xe <= dim_x
eys < ye <= dim_y
zs < ze <= dim_z
```

越界会返回错误码或抛异常，不会静默裁剪。

### 1.4 切片和结果布局

公共 API 固定如下输出布局：

| API | shape | flat offset |
|---|---:|---|
| X slice `read_slice(x, i)` | `(dim_y, dim_z)` | `y * dim_z + z` |
| Y slice `read_slice(y, i)` | `(dim_x, dim_z)` | `x * dim_z + z` |
| Z slice `read_slice(z, i)` | `(dim_x, dim_y)` | `x * dim_y + y` |
| X column | `(dim_x)` | `x` |
| Y column | `(dim_y)` | `y` |
| Z column | `(dim_z)` | `z` |
| Subvolume | `(xe-xs, ye-ys, ze-zs)` | `(x-xs) * ny * nz + (y-ys) * nz + (z-zs)` |

所有数值均为 `float32`。

---

## 2. 构建与安装

### 2.1 普通 C/C++ 构建

构建系统已按组件拆分为多个 CMake 子工程，但外部命令、选项和目标名保持不变：顶层 `CMakeLists.txt` 负责统一编排，`src/lib/` 构建 `block3d`，`src/cli/` 构建 `block3d_cli`，`src/benchmark/` 构建 `run_test` 和 benchmark 私有库，`tests/`、`examples/`、`python/` 分别管理对应目标。

```bash
cmake -S <提交包目录>/source/block3d-cpp -B <提交包目录>/build -DCMAKE_BUILD_TYPE=Release -DBLOCK3D_BUILD_TESTS=ON -DBLOCK3D_BUILD_EXAMPLES=ON
cmake --build <提交包目录>/build --config Release
ctest --test-dir <提交包目录>/build -C Release --output-on-failure
```

### 2.2 安装到本地 prefix

```bash
cmake --install <提交包目录>/build --prefix <提交包目录>/install
```

安装后布局大致为：

```text
install/
  include/block3d/
    block3d.h
    block3d.hpp
    version.h
    core.hpp
    converter.hpp
    reader.hpp
    rng.hpp
    types.hpp
  bin/
    block3d_cli.exe
    run_test.exe
  lib/
    block3d.lib
  lib/cmake/block3d/
    block3dConfig.cmake
    block3dConfigVersion.cmake
    block3dTargets.cmake
```

### 2.3 包外 CMake 消费

安装后，新项目可以这样使用：

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_block3d_app LANGUAGES CXX)

find_package(block3d CONFIG REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE block3d::block3d)
```

配置时传入安装前缀：

```bash
cmake -S my_app -B my_app/build -DCMAKE_PREFIX_PATH=<提交包目录>/install
cmake --build my_app/build --config Release
```

---

## 3. C++ API 使用

C++ 推荐使用：

```cpp
#include <block3d/block3d.hpp>
```

核心类型：

```cpp
block3d::ConvertOptionsFacade
block3d::ReaderOptionsFacade
block3d::Reader
block3d::Array
block3d::SliceBatch
```

### 3.1 转换 raw -> b3d

```cpp
#include <block3d/block3d.hpp>

int main() {
    block3d::ConvertOptionsFacade options;
    options.block_size = 0;              // 0 = 自动探测存储并选择 block_size
    options.num_threads = 0;             // 0 = auto
    options.max_memory_mb = 0;           // 软预算，不是进程级硬上限
    options.layout = block3d::Layout::LegacyXYZ;
    options.micro_size = 0;
    options.progress = true;

    block3d::convert_raw_to_b3d(
        "<数据目录>/test18.dat",
        "<数据目录>/test18_api.b3d",
        801, 2405, 2501,
        options);
}
```

如果需要生成 micro-tiled v2：

```cpp
block3d::ConvertOptionsFacade options;
options.block_size = 128;                        // 后期 test18 v2 正式 API 测试建议显式使用 128
options.layout = block3d::Layout::MicroTiledXYZ;
options.micro_size = 8;
options.num_threads = 0;
options.progress = true;

block3d::convert_raw_to_b3d(raw, b3d, dx, dy, dz, options);
```

后期正式 API 测试建议显式使用 micro-tiled v2，并保持 C++ / Python 两边参数一致。例如 test18 可使用 `block_size=128, layout=micro-tiled, micro_size=8`。

### 3.2 打开 `.b3d` 并读取 info

```cpp
block3d::ReaderOptionsFacade reader_options;
reader_options.num_threads = 0;
reader_options.max_memory_mb = 0;
reader_options.read_dispatch = block3d::ReadDispatch::RoundRobin;

block3d::Reader reader("<数据目录>/test18_api.b3d", reader_options);
block3d::FileInfo info = reader.info();

std::cout << info.dim_x << " " << info.dim_y << " " << info.dim_z << "\n";
std::cout << "block_size=" << info.block_size << "\n";
std::cout << "format_version=" << info.format_version << "\n";
```

### 3.3 单切片读取

```cpp
auto x0 = reader.read_slice(block3d::Axis::X, 0);
// x0.shape == {dim_y, dim_z}
// x0.data[y * dim_z + z]

float v = x0.data[100 * info.dim_z + 200];
```

Y / Z 切片：

```cpp
auto y0 = reader.read_slice(block3d::Axis::Y, 0); // shape = {dim_x, dim_z}
auto z0 = reader.read_slice(block3d::Axis::Z, 0); // shape = {dim_x, dim_y}
```

### 3.4 批量切片读取

```cpp
std::vector<std::uint64_t> indices = {0, 1, 2, 100};
auto batch = reader.read_slices(block3d::Axis::X, indices);

// batch.data 是连续存储：slice_count * slice_elems
// 第 i 张切片起点：
const float* slice_i = batch.data.data() + i * batch.slice_elems;
```

shape 信息：

```cpp
batch.slice_count;      // indices.size()
batch.slice_elems;      // 单张切片元素数
batch.slice_shape;      // X: {dim_y, dim_z}, Y: {dim_x, dim_z}, Z: {dim_x, dim_y}
```

### 3.5 批量切片写出为 `.raw`

```cpp
#include <fstream>

for (std::size_t i = 0; i < batch.slice_count; i++) {
    const float* slice = batch.data.data() + i * batch.slice_elems;

    std::ofstream out("slice.raw", std::ios::binary);
    out.write(reinterpret_cast<const char*>(slice),
              static_cast<std::streamsize>(batch.slice_elems * sizeof(float)));
}
```

写出的 `.raw` 是无头 `float32`，布局就是对应切片的公共返回布局。

### 3.6 X 主维单列读取

项目需求中特别关注 X 主维单列：固定 `(y, z)`，沿 X 读取：

```cpp
auto xcol = reader.read_column(block3d::Axis::X, 0, 0);
// xcol.shape == {dim_x}
// xcol.data[x] == raw[x, 0, 0]
```

Y/Z 列：

```cpp
auto ycol = reader.read_column(block3d::Axis::Y, 0, 0); // fixed (x, z), length dim_y
auto zcol = reader.read_column(block3d::Axis::Z, 0, 0); // fixed (x, y), length dim_z
```

### 3.7 子体积和单点

```cpp
auto sub = reader.read_subvolume(0, 4, 0, 5, 0, 6);
// shape = {4, 5, 6}
// offset = x * 5 * 6 + y * 6 + z，均为局部坐标

float p = reader.read_point(0, 0, 0);
```

### 3.8 raw 随机点校验

```cpp
bool ok = reader.verify("<数据目录>/test18.dat", 1000, 1e-3f);
if (!ok) {
    throw std::runtime_error("verify failed");
}
```

注意：`verify()` 会读取 raw 文件，会污染 OS 页缓存。不要在正式 cold benchmark 前同进程调用它。

---

## 4. C API 使用

C API 头文件：

```c
#include <block3d/block3d.h>
```

C API 是 ABI 边界，所有函数返回 `block3d_status`，不会让 C++ 异常穿过 C ABI。

### 4.1 转换 raw -> b3d

```c
block3d_convert_options options = block3d_default_convert_options();
options.block_size = 0;  /* 0 = auto */
options.num_threads = 0;
options.layout = BLOCK3D_LAYOUT_LEGACY_XYZ;
options.micro_size = 0;
options.progress = 1;

block3d_status st = block3d_convert_raw_to_b3d(
    "<数据目录>/test18.dat",
    "<数据目录>/test18_api.b3d",
    801, 2405, 2501,
    &options);

if (st != BLOCK3D_OK) {
    fprintf(stderr, "convert failed: %s\n", block3d_status_message(st));
}
```

micro-tiled v2：

```c
options.block_size = 128;
options.layout = BLOCK3D_LAYOUT_MICRO_TILED_XYZ;
options.micro_size = 8;
```

### 4.2 打开和关闭

```c
block3d_reader_options ro = block3d_default_reader_options();
ro.num_threads = 0;
ro.read_dispatch = BLOCK3D_READ_DISPATCH_ROUND_ROBIN;

block3d_context* ctx = NULL;
block3d_status st = block3d_open_b3d("test18_api.b3d", &ro, &ctx);
if (st != BLOCK3D_OK) {
    fprintf(stderr, "open failed: %s\n", block3d_status_message(st));
    return 1;
}

/* ... use ctx ... */

block3d_close(ctx);
```

### 4.3 读取 info

```c
block3d_file_info info;
block3d_status st = block3d_get_info(ctx, &info);

printf("dims=%llu x %llu x %llu\n",
       (unsigned long long)info.dim_x,
       (unsigned long long)info.dim_y,
       (unsigned long long)info.dim_z);
```

### 4.4 读取单切片

```c
block3d_array arr = {0};
block3d_status st = block3d_read_slice(ctx, BLOCK3D_AXIS_X, 0, &arr);
if (st == BLOCK3D_OK) {
    /* X slice: arr.ndim=2, arr.dim0=dim_y, arr.dim1=dim_z */
    float v = arr.data[100 * arr.dim1 + 200];
}
block3d_free_array(&arr);
```

C API 返回数组由库分配，必须用：

```c
block3d_free_array(&arr);
```

释放。

### 4.5 批量切片

```c
uint64_t indices[] = {0, 1, 2, 100};
block3d_slice_batch batch = {0};

block3d_status st = block3d_read_slices_batch(
    ctx,
    BLOCK3D_AXIS_X,
    indices,
    4,
    &batch);

if (st == BLOCK3D_OK) {
    /* batch.data 连续存储：slice_count * slice_elems */
    const float* slice0 = batch.data;
    const float* slice1 = batch.data + batch.slice_elems;
}

block3d_free_slice_batch(&batch);
```

### 4.6 列、子体积、单点和 verify

```c
block3d_array col = {0};
block3d_read_column(ctx, BLOCK3D_AXIS_X, 0, 0, &col);
block3d_free_array(&col);

block3d_array sub = {0};
block3d_read_subvolume(ctx, 0, 4, 0, 5, 0, 6, &sub);
block3d_free_array(&sub);

float value = 0.0f;
block3d_read_point(ctx, 0, 0, 0, &value);

block3d_status verify_st = block3d_verify_points(ctx, raw_path, 1000, 1e-3f);
```

### 4.7 C API 错误码

常见状态：

| 状态 | 说明 |
|---|---|
| `BLOCK3D_OK` | 成功 |
| `BLOCK3D_ERROR_INVALID_ARGUMENT` | 空指针、非法参数、非法 axis 等 |
| `BLOCK3D_ERROR_IO` | 文件打开/映射/写入失败 |
| `BLOCK3D_ERROR_OUT_OF_MEMORY` | 内存分配失败 |
| `BLOCK3D_ERROR_OUT_OF_RANGE` | 坐标或切片 index 越界 |
| `BLOCK3D_ERROR_FORMAT` | `.b3d` magic/version/layout/header 错误 |
| `BLOCK3D_ERROR_INTERNAL` | 其它内部错误 |

可以用：

```c
block3d_status_message(st)
```

转成人可读字符串。

---

## 5. Python API 使用

Python 模块名：

```python
import block3d
```

返回值使用 NumPy `ndarray(dtype=float32)`。

### 5.1 构建 Python 模块

需要 `pybind11` 和 `numpy`。如果 base conda 环境不可写，可以创建独立环境：

```bash
conda create -y -p <Python环境目录>/block3d-py python=3.13 pybind11 numpy
```

配置和构建：

```bash
cmake -S <提交包目录>/source/block3d-cpp -B <提交包目录>/build_py ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DBLOCK3D_BUILD_PYTHON=ON ^
  -DBLOCK3D_BUILD_TESTS=OFF ^
  -DBLOCK3D_BUILD_EXAMPLES=OFF ^
  -DBLOCK3D_BUILD_CLI=OFF ^
  -DBLOCK3D_BUILD_RUN_TEST=OFF ^
  -DPython3_EXECUTABLE=<Python环境目录>/block3d-py/python.exe ^
  -Dpybind11_DIR=<Python环境目录>/block3d-py/Lib/site-packages/pybind11/share/cmake/pybind11

cmake --build <提交包目录>/build_py --config Release
```

生成文件类似：

```text
<提交包目录>/build_py/Release/block3d.cp313-win_amd64.pyd
```

临时运行时可设置：

```bash
set PYTHONPATH=<提交包目录>/build_py/Release
python -c "import block3d; print(block3d.__version__)"
```

Git Bash / conda run 示例：

```bash
conda run -p <Python环境目录>/block3d-py python -c "import sys; sys.path.insert(0, r'<提交包目录>/build_py/Release'); import block3d; print(block3d.__version__)"
```

### 5.2 转换 raw -> b3d

```python
import block3d

block3d.convert_raw_to_b3d(
    raw_path="<数据目录>/test18.dat",
    b3d_path="<数据目录>/test18_api_py.b3d",
    dim_x=801,
    dim_y=2405,
    dim_z=2501,
    block_size=0,          # 0 = auto
    layout="legacy",
    threads=0,
    memory_limit_mb=0,
    progress=True,
)
```

micro-tiled v2：

```python
block3d.convert_raw_to_b3d(
    raw_path="<数据目录>/test18.dat",
    b3d_path="<数据目录>/test18_micro8_api_py.b3d",
    dim_x=801,
    dim_y=2405,
    dim_z=2501,
    block_size=128,
    layout="micro-tiled",
    micro_size=8,
    threads=0,
    progress=True,
)
```

### 5.3 打开和读取 info

```python
reader = block3d.Reader(
    "<数据目录>/test18_api_py.b3d",
    threads=0,
    memory_limit_mb=0,
    read_dispatch="round-robin",
)

info = reader.info()
print(info)
```

`info` 是 dict，字段包括：

```python
{
    "dim_x": ...,
    "dim_y": ...,
    "dim_z": ...,
    "block_size": ...,
    "total_blocks": ...,
    "data_offset": ...,
    "format_version": ...,
    "layout": "legacy" 或 "micro-tiled",
    "micro_size": ...,
}
```

### 5.4 读取切片

```python
x0 = reader.read_slice("x", 0)
print(x0.shape, x0.dtype)  # (dim_y, dim_z), float32

value = float(x0[100, 200])
```

Y/Z：

```python
y0 = reader.read_slice("y", 0)  # shape = (dim_x, dim_z)
z0 = reader.read_slice("z", 0)  # shape = (dim_x, dim_y)
```

### 5.5 批量切片

```python
indices = [0, 1, 2, 100]
batch = reader.read_slices("x", indices)
print(batch.shape)  # (n, dim_y, dim_z)

slice0 = batch[0]
slice1 = batch[1]
```

写出每张切片：

```python
from pathlib import Path

out_dir = Path("api_output")
out_dir.mkdir(parents=True, exist_ok=True)

for request_pos, index in enumerate(indices):
    out = out_dir / f"x_{request_pos:04d}_{index}.raw"
    batch[request_pos].astype("float32", copy=False).tofile(out)
```

### 5.6 X 主维单列、子体积、单点

```python
xcol = reader.read_column("x", 0, 0)
print(xcol.shape)  # (dim_x,)

sub = reader.read_subvolume(0, 4, 0, 5, 0, 6)
print(sub.shape)   # (4, 5, 6)

p = reader.read_point(0, 0, 0)
```

### 5.7 verify

```python
ok = reader.verify("<数据目录>/test18.dat", samples=1000, tolerance=1e-3)
print(ok)
```

同样注意：`verify()` 会读取 raw 文件，会影响 OS 页缓存状态，不要和正式 cold benchmark 混在一起。

---

## 6. 正式 API 测试脚本：api_random_sequential

仓库中提供两个后期主要正式测试脚本，均只通过公开 API 工作：

```text
C++:    examples/cpp/api_random_sequential.cpp
Python: examples/python/api_random_sequential.py
```

两者流程一致：

1. raw `float32 .dat` 转换为 `.b3d`，如果目标 `.b3d` 已存在且参数匹配则跳过转换；
2. 打开 `.b3d` 并打印维度、block size、format version、layout、micro size；
3. 可选随机点 verify；
4. 对 X/Y/Z 三轴分别执行 random 和 sequential 切片读取；
5. 每张切片写出为无头 `float32 .raw`；
6. 输出每轴每模式的读取耗时、写盘耗时、总耗时、吞吐量、checksum 和计划哈希；
7. 额外演示 X 主维单列、子体积和单点读取。

> 这两个脚本不做 cold/hot cache scrub。它们是 API 读写验收计时脚本，成绩受当前 OS 页缓存和磁盘状态影响；配置更严格的 `run_test.exe` 仅作为开发团队内部 cold/hot benchmark 工具。

### 6.1 生成 test18 原始数据

如没有官方 `test18.dat`，可用提交包自带 `generator_py` 生成无头 `float32`：

```powershell
Set-Location "<提交包目录>"

python .\source\block3d-cpp\generator_py\run.py --cli `
  -x 801 `
  -y 2405 `
  -z 2501 `
  --min 0 `
  --max 1 `
  --engine numpy `
  --no-header `
  -o .\work\test18_v2\test18.dat
```

`--no-header` 必须保留。`test18.dat` 约 17.95 GiB。

### 6.2 v2 micro-tiled 参数

这里的 “v2” 是转换后的 `.b3d` 文件格式：

```text
format_version = 2
layout = micro-tiled
micro_size = 8
```

不是 `generator_py` 的数据格式。正式 API 测试建议显式使用：

```text
--layout micro-tiled --micro-size 8 --block-size 128
```

### 6.3 C++ 示例构建

```powershell
Set-Location "<提交包目录>"

cmake -S .\source\block3d-cpp -B .\build_api_examples `
  -DBLOCK3D_BUILD_TESTS=OFF `
  -DBLOCK3D_BUILD_CLI=OFF `
  -DBLOCK3D_BUILD_RUN_TEST=OFF `
  -DBLOCK3D_BUILD_EXAMPLES=ON `
  -DBLOCK3D_BUILD_PYTHON=OFF

cmake --build .\build_api_examples --config Release --target api_random_sequential_cpp
```

生成：

```text
<提交包目录>\build_api_examples\Release\api_random_sequential_cpp.exe
```

### 6.4 C++ test18 v2 正式命令

```powershell
Set-Location "<提交包目录>"

.\build_api_examples\Release\api_random_sequential_cpp.exe `
  --raw .\work\test18_v2\test18.dat `
  --b3d .\work\test18_v2\test18_v2.b3d `
  --dim-x 801 `
  --dim-y 2405 `
  --dim-z 2501 `
  --layout micro-tiled `
  --micro-size 8 `
  --block-size 128 `
  --threads 16 `
  --memory-limit 1024 `
  --random-count 100 `
  --seq-count 10 `
  --batch-window 1 `
  --verify-samples 1000 `
  --output-dir .\work\test18_v2\api_example_output_cpp
```

开头应看到：

```text
API_CONVERT_RESULT raw=... b3d=... dims=801x2405x2501 block=128 version=2 layout=micro-tiled micro_size=8 convert_sec=...
API_VERIFY_RESULT ok=1 samples=1000 verify_sec=...
```

### 6.5 Python binding 构建

建议在提交包内创建独立 venv：

```powershell
Set-Location "<提交包目录>"

python -m venv .\work\py_block3d_venv
.\work\py_block3d_venv\Scripts\python.exe -m pip install --upgrade pip
.\work\py_block3d_venv\Scripts\python.exe -m pip install numpy pybind11

$Pybind11Dir = .\work\py_block3d_venv\Scripts\python.exe -m pybind11 --cmakedir

cmake -S .\source\block3d-cpp -B .\build_python `
  -DBLOCK3D_BUILD_TESTS=OFF `
  -DBLOCK3D_BUILD_CLI=OFF `
  -DBLOCK3D_BUILD_RUN_TEST=OFF `
  -DBLOCK3D_BUILD_EXAMPLES=OFF `
  -DBLOCK3D_BUILD_PYTHON=ON `
  -DPython3_EXECUTABLE=".\work\py_block3d_venv\Scripts\python.exe" `
  -Dpybind11_DIR="$Pybind11Dir"

cmake --build .\build_python --config Release --target block3d_python
```

运行前设置并检查 `PYTHONPATH`：

```powershell
$env:PYTHONPATH = Join-Path (Get-Location).Path "build_python\Release"
.\work\py_block3d_venv\Scripts\python.exe -c "import block3d; print(block3d.__file__)"
```

输出应指向本提交包的 `build_python\Release\block3d...pyd`。如果导入到其它目录，可能会加载机器上已有的同名包，导致 `Reader` 不存在。

### 6.6 Python test18 v2 正式命令

如果 C++ 已经生成 `.\work\test18_v2\test18_v2.b3d`，Python 可复用同一份 v2 文件。

```powershell
Set-Location "<提交包目录>"
$env:PYTHONPATH = Join-Path (Get-Location).Path "build_python\Release"

.\work\py_block3d_venv\Scripts\python.exe .\source\block3d-cpp\examples\python\api_random_sequential.py `
  --raw .\work\test18_v2\test18.dat `
  --b3d .\work\test18_v2\test18_v2.b3d `
  --dim-x 801 `
  --dim-y 2405 `
  --dim-z 2501 `
  --layout micro-tiled `
  --micro-size 8 `
  --block-size 128 `
  --threads 16 `
  --memory-limit 1024 `
  --random-count 100 `
  --seq-count 10 `
  --batch-window 1 `
  --verify-samples 1000 `
  --output-dir .\work\test18_v2\api_example_output_py
```

应看到：

```text
格式版本=2 布局=micro-tiled 微块大小=8
API验证结果 是否通过=1
```

### 6.7 输出字段说明

| 字段 | 说明 |
|---|---|
| `读取总耗时` / `read_sec` | `read_slices()` 调用累计耗时 |
| `写盘总耗时` / `write_sec` | 写出 `.raw` 文件耗时累计 |
| `总耗时` / `total_sec` | 从读取 case 开始到该 case 全部写完的 wall time |
| `输出MiB` | 输出切片总 MiB |
| `吞吐量` | `输出MiB / 总耗时` |
| `计划哈希` / `plan_hash` | 请求计划哈希，用于确认 random/sequential 索引计划一致性 |
| `校验和` / `checksum` | 轻量抽样 checksum，用于确认数据被读取，不能替代 verify |

正式汇报建议记录四组 `API模式汇总`：C++ 随机、C++ 顺序、Python 随机、Python 顺序。

---

## 7. 推荐调用模式

### 7.1 正式 API 验收模式

用于后期主要测试：

1. 使用 `generator_py` 或官方数据准备无头 `test18.dat`；
2. 使用 `api_random_sequential_cpp` 转换/读取/写盘，并确认 `version=2 layout=micro-tiled micro_size=8`；
3. 使用 `api_random_sequential.py` 复用同一 `.b3d` 或重新转换同配置 `.b3d`；
4. 保留两边完整控制台输出；
5. 汇总 `API模式汇总` 的三轴平均读取、写盘、总耗时和总体吞吐量。

### 7.2 大数据输出模式

用于产出大量切片文件：

1. 提前确认输出目录磁盘空间；
2. 使用 `read_slices()` 分窗口读取，避免一次性 batch 太大；
3. 每个 batch 读完立即写出；
4. 记录 `read_sec` / `write_sec` / `total_sec`；
5. 重复测试前清理旧输出目录，避免混淆。

### 7.3 严格 cold/hot benchmark 模式

`run_test.exe` 和 `block3d_cli bench` 需要配置 scrub 文件、cold/hot cache 口径、输出同步和日志归档。由于配置严格，甲方后期主验收不要求直接使用；仅在开发团队需要 cold/hot 对比时参考 `<提交包目录>/docs/基准操作手册.md` 附录。

---

## 8. 常见问题

### 8.1 `block_size=0` 是什么？

`block_size=0` 表示自动选择块大小。转换时会探测输出目录所在存储介质，并用 `auto_block_size()` 根据维度选择块大小。

如果要复现已有 `.b3d` 配置，建议显式传相同 block size。后期 test18 v2 正式 API 测试建议使用：

```text
block_size = 128
layout = micro-tiled
micro_size = 8
```

### 8.2 `max_memory_mb` / `memory_limit_mb` 是硬限制吗？

不是。它是转换 / 读取路径内部的软预算，不是整个进程的内存硬上限。

### 8.3 Python 为什么必须返回 NumPy？

大切片可能几十 MB。返回 Python list 会产生巨大对象开销；NumPy `ndarray(float32)` 能直接表达 shape、dtype 和连续内存布局。

### 8.4 `.raw` 输出能直接和原始 `.dat` 比吗？

切片 `.raw` 是二维切片，不是完整三维原始 `.dat`。它的布局按本文第 1.4 节：

```text
X slice -> Y-Z
Y slice -> X-Z
Z slice -> X-Y
```

如果要按坐标对照 raw，需要用相应 flat offset 计算。

### 8.5 C API 返回的数据什么时候释放？

只要 C API 填充了：

```c
block3d_array
block3d_slice_batch
```

调用方都应调用：

```c
block3d_free_array(&array);
block3d_free_slice_batch(&batch);
```

即使失败路径中结构体为空，调用 free 函数也是安全的。

### 8.6 Windows 下 Python 找不到 `block3d` 怎么办？

确认 `.pyd` 所在目录在 `PYTHONPATH`：

```bash
set PYTHONPATH=<提交包目录>/build_py/Release
python -c "import block3d; print(block3d.__version__)"
```

如果使用 conda run，可以在脚本里临时插入：

```python
import sys
sys.path.insert(0, r"<提交包目录>/build_py/Release")
import block3d
```

---

## 9. 最小 C++ 完整示例

```cpp
#include <block3d/block3d.hpp>
#include <fstream>
#include <iostream>

int main() {
    block3d::ConvertOptionsFacade conv;
    conv.block_size = 128;
    conv.layout = block3d::Layout::MicroTiledXYZ;
    conv.micro_size = 8;
    conv.num_threads = 0;
    conv.progress = true;

    block3d::convert_raw_to_b3d(
        "<数据目录>/test18.dat",
        "<数据目录>/test18_api_micro8.b3d",
        801, 2405, 2501,
        conv);

    block3d::Reader reader("<数据目录>/test18_api_micro8.b3d");
    auto info = reader.info();
    std::cout << info.dim_x << "x" << info.dim_y << "x" << info.dim_z << "\n";

    auto xs = reader.read_slices(block3d::Axis::X, {0, 1, 2});
    for (std::uint64_t i = 0; i < xs.slice_count; i++) {
        const float* slice = xs.data.data() + i * xs.slice_elems;
        std::ofstream out("x_slice.raw", std::ios::binary);
        out.write(reinterpret_cast<const char*>(slice),
                  static_cast<std::streamsize>(xs.slice_elems * sizeof(float)));
    }

    auto xcol = reader.read_column(block3d::Axis::X, 0, 0);
    auto sub = reader.read_subvolume(0, 4, 0, 4, 0, 4);
    float p = reader.read_point(0, 0, 0);

    std::cout << "xcol_len=" << xcol.shape[0] << " point=" << p << "\n";
    return 0;
}
```

---

## 10. 最小 Python 完整示例

```python
import block3d

block3d.convert_raw_to_b3d(
    raw_path="<数据目录>/test18.dat",
    b3d_path="<数据目录>/test18_api_micro8_py.b3d",
    dim_x=801,
    dim_y=2405,
    dim_z=2501,
    block_size=128,
    layout="micro-tiled",
    micro_size=8,
    threads=0,
    progress=True,
)

reader = block3d.Reader("<数据目录>/test18_api_micro8_py.b3d")
print(reader.info())

batch = reader.read_slices("x", [0, 1, 2])
print(batch.shape, batch.dtype)

for i, index in enumerate([0, 1, 2]):
    batch[i].tofile(f"x_{index}.raw")

xcol = reader.read_column("x", 0, 0)
sub = reader.read_subvolume(0, 4, 0, 4, 0, 4)
p = reader.read_point(0, 0, 0)
print(xcol.shape, sub.shape, p)
```
