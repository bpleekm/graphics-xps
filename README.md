# graphics-xps

用 **C11 + 可扩展GPU后端**探测FP／INT算术支持，并以分块矩阵乘法测试实测算力。
当前实现OpenCL后端，精度支持由运行时能力查询和数值自检确定。
AMD、NVIDIA、macOS ARM64的扩展边界及验证状态见[后端设计](docs/architecture.md)。

**默认自动检测精度，逐项串行测试。`-m`是最大GPU数据预算，不要求用满。**
终端结果默认输出对齐ASCII表格；`-v`可在stderr显示初始化信息与逐秒进度，JSON保留完整明细。

## 目录

1. [安装](#安装)
2. [从源码构建](#从源码构建)
3. [检查GPU访问权限](#检查gpu访问权限)
4. [完整运行流程](#完整运行流程)
5. [短参数参考](#短参数参考)
6. [精度语义与自动检测](#精度语义与自动检测)
7. [内存预算与矩阵尺寸](#内存预算与矩阵尺寸)
8. [计时与算力口径](#计时与算力口径)
9. [结果与常见问题](#结果与常见问题)

## 安装

需要C11编译器、Make和OpenCL开发库；执行GPU测试还需要兼容的OpenCL运行时。
以下为通用安装示例，不代表任何实际测试设备或环境清单。验证方法见[`docs/validation/`](docs/validation/)。

### Ubuntu构建依赖

```sh
sudo apt-get update
sudo apt-get install -y build-essential pkg-config ocl-icd-opencl-dev
```

运行需要OpenCL ICD loader及兼容的GPU运行时。构建完成后用程序的`probe`入口检查可用性；仅运行二进制不需要开发头文件或pkg-config。

在提供以下软件包的Linux发行版上可安装OpenCL loader，并按设备供应商要求安装兼容的运行时：

```sh
sudo apt-get install -y ocl-icd-libopencl1
```

运行时版本由设备供应商兼容要求决定。诊断输出仅供本地排查，不应未经检查直接公开。

### macOS构建入口

```sh
xcode-select --install
make CC=clang
make CC=clang check
```

Makefile自动使用`-framework OpenCL`及Apple头文件。此处提供构建路径，**macOS ARM64 GPU运行尚未真机验证**；后续可通过C ABI接入Metal后端。
AMD／NVIDIA使用厂商提供的OpenCL运行时，也尚未在本项目真机验证；当前未实现CUDA／HIP后端。

## 从源码构建

```sh
git clone https://github.com/bpleekm/graphics-xps.git
cd graphics-xps
make -j2
make check
./build/graphics-xps -h
```

按仓库可见性使用已有的GitHub访问方式。

- `make`只构建，不启动GPU测试。
- 主程序为`build/graphics-xps`，运行时不依赖Python、C++或外部内核文件。
- OpenCL内核通过一个小型C构建工具嵌入二进制。
- 默认不使用`-march=native`，避免把构建产物绑定到特定CPU指令集。
- `-j2`只用于编译；程序内的GPU测试一直串行。

### Makefile目标

| 目标 | 行为 |
|---|---|
| `make` / `make all` | 构建C程序 |
| `make check` | CPU上的参数、预算、半精度转换及计数测试，不需要GPU |
| `make probe` | 自动检测并执行小矩阵正确性探测 |
| `make smoke` | 自动精度、`N=37`、每项0.05秒，验证非整tile边界 |
| `make bench` | 自动精度正式测试 |
| `make report-check REPORT=path.json` | 用可选Python标准库工具复核JSON计数 |
| `make privacy-check` | 隐私回归测试及待发布工作文件检查 |
| `make privacy-history` | 额外检查所有可达Git提交的内容与提交身份 |
| `make clean` | 删除构建产物，保留测试报告 |
| `make help` | 显示常用入口 |

可覆盖变量：

```sh
make CC=clang BUILD_DIR=build/clang -j2
make CC=clang BUILD_DIR=build/clang check
make bench MEMORY=1G SECONDS=10 ARGS='-o results/run.json'
```

切换同一构建目录的编译器或CFLAGS前执行`make clean`，或使用独立`BUILD_DIR`。
Makefile目前支持`BACKEND=opencl`；新增后端的注册与链接位置见[设计文档](docs/architecture.md)。

## 检查GPU访问权限

Linux进程需要对应render节点的读写权限，通常来自`render`组或会话ACL。
若普通用户的`probe`不可用，检查运行时及设备节点权限，不需要导出账号或设备清单。

如果当前账号缺少render访问权，可由sudo临时赋予**本次进程**render补充组，例如在仓库目录执行：

```sh
CURRENT_USER=$(id -un)
CURRENT_GROUP=$(id -gn)
sudo runuser -u "$CURRENT_USER" -g "$CURRENT_GROUP" -G render -- ./build/graphics-xps probe -pauto
sudo runuser -u "$CURRENT_USER" -g "$CURRENT_GROUP" -G render -- make smoke
```

以下主流程假定当前进程已经具备render访问权；必要时给命令添加上述`sudo runuser ... --`前缀。
构建产物和测试文件由所选账号创建。

## 完整运行流程

### 1. 核心检查

```sh
make
make check
mkdir -p results
```

### 2. 自动能力探测

```sh
./build/graphics-xps probe -pauto -o results/probe.json
```

逐项查询能力、编译内核，并验证最多32×32的小矩阵。
`probe`不执行`-t`时长的性能负载，也不把小矩阵探测结果当作算力成绩。

### 3. 小矩阵正确性检查

```sh
make smoke ARGS='-o results/smoke.json'
```

`N=37`用于检查矩阵边界、K维尾部以及各精度的输出类型。

### 4. 自动精度，每项10秒，最大GPU预算1 GiB

```sh
./build/graphics-xps bench -pauto -t10 -m1G -o results/auto-10s-1g.json
```

需要临时render访问权时，执行：

```sh
sudo runuser -u "$(id -un)" -g "$(id -gn)" -G render -- \
  ./build/graphics-xps bench -pauto -t10 -m1G -o results/auto-10s-1g.json
```

自动模式依次检查全部精度；没有声明所需能力的项目会跳过。
各项严格顺序执行；实际总时间还包括编译、初始化、校准／预热和校验。

### 5. 核对报告

```sh
python3 tools/check_report.py results/auto-10s-1g.json
# 或
make report-check REPORT=results/auto-10s-1g.json
```

Python仅用于报告校验、隐私检查等开发辅助工具，不是C程序运行依赖。

`-o`不会覆盖已有文件；重复运行使用新文件名，例如：

```sh
./build/graphics-xps bench -pauto -m1G -t10 \
  -o "results/run-$(date -u +%Y%m%dT%H%M%SZ).json"
```

## 短参数参考

命令形式：`graphics-xps [bench|probe] [短参数]`。省略子命令时默认`bench`；完全不带参数只显示帮助。

| 参数 | 默认值 | 说明 |
|---|---|---|
| `-m SIZE` | `256M` | GPU显式数据缓冲区最大预算，正整数，可用K/M/G或KiB/MiB/GiB |
| `-p LIST` | `auto` | `fp16,fp32,fp64,int8,int16,int32,int64`的筛选列表；`auto/all`自动检测全部 |
| `-t SECONDS` | `10` | 每精度正式阶段的默认墙钟秒数，范围0.01–86400 |
| `-t name=SECONDS,...` | 无 | 每精度时长覆盖，可重复，覆盖值优先于全局默认 |
| `-n N` | 自动 | 手动平方矩阵边长，1–32768；不得超过预算或设备限制 |
| `-b NAME` | `auto` | 后端；当前`auto`选择`opencl` |
| `-d INDEX` | `0` | 后端枚举的GPU序号，不枚举CPU设备 |
| `-w SECONDS` | `0.2` | 每精度校准后的预热时间，0–60秒，不计入`-t` |
| `-o FILE` | 不写JSON | 新报告路径，父目录必须存在 |
| `-V` | — | 版本 |
| `-v` | 关闭 | 在stderr显示详细初始化、逐秒进度与完整内核路径说明 |
| `-h` | — | 帮助 |

`-m256M`、`-m 256M`等价；`M=2²⁰`字节，`G=2³⁰`字节。仅支持整数容量，不接受`1.5G`。

### 省略`-p`，自动检测

```sh
./build/graphics-xps -m256M -t5
```

### 分精度时长

```sh
./build/graphics-xps bench -pauto -m1G -t10 -t fp16=20,fp32=30,int8=15
```

其他精度10秒，FP16为20秒，FP32为30秒，INT8为15秒。
覆盖值与全局默认出现的顺序无关；同一精度多次覆盖时最后一次生效。

### 手动矩阵尺寸

```sh
./build/graphics-xps bench -pauto -m256M -n2048 -t5
```

### 仅筛选部分精度

```sh
./build/graphics-xps bench -p fp32,int8 -m256M -t5
```

筛选不会绕过能力探测或数值自检，也不会把不支持的精度悄悄换成CPU／其他浮点精度。

## 精度语义与自动检测

| 名称 | 输入 | 累加／输出 | 路径 |
|---|---|---|---|
| FP16 | half | half | half FMA，要求FP16算术扩展 |
| FP32 | float | float | float FMA |
| FP64 | double | double | double FMA，要求FP64能力 |
| INT8 | 有符号8位 | INT32 | 有扩展时char4 dot；否则明确标注widened通用实现 |
| INT16 | 有符号16位 | INT32 | 明确标注widened通用实现 |
| INT32 | 有符号32位 | INT32 | 整数乘加 |
| INT64 | 有符号64位 | INT64 | 整数乘加 |

程序分别报告`declared_supported`、`compiled`、`self_test_passed`、`verified`和`kernel_path`。

- `unsupported`表示驱动没有声明所需算术能力，自动模式会记录原因并跳过。
- 驱动声明支持但内核编译／校验失败，记为失败，不改称unsupported。
- INT8 dot是可选加速；扩展内核编译失败时允许切换到**明确标注**的widened路径，并记录原因。
- INT8/INT16是窄输入、宽累加的数学吞吐，不据此宣称纯8／16位ALU峰值。

## 内存预算与矩阵尺寸

`-m`限制同时存活的**显式GPU数据缓冲区**：当前内核只有A、B、C，未来后端增加工作区也必须计入。

```text
A = N² × 输入元素字节数
B = N² × 输入元素字节数
C = N² × 累加元素字节数
总量 = A + B + C（+ 未来后端的显式工作区） ≤ -m
```

每个精度完成后释放其数据，再分配下一个精度。小矩阵自检也在预算内，且先释放再开始大矩阵。

自动尺寸按16对齐，默认最多4096；另检查单次分配上限，并将有效预算限制在设备声明全局内存的一半以内。
预算是上限，**不追求占满**。`-m1G`且N=4096时：

| 精度 | A+B+C |
|---|---:|
| FP16／INT8 | 96 MiB |
| FP32／INT32 | 192 MiB |
| FP64／INT64 | 384 MiB |
| INT16 | 128 MiB |

需要不同维度时使用`-n`，程序会严格检查它是否仍在预算内。
CPU初始化使用1 MiB临时缓冲，不保存三份完整CPU矩阵；大矩阵最终只抽取校验单元。
CPU内存、编译器／驱动隐藏分配、GPU寄存器／内部scratch等不属于这个显式数据预算；进程峰值RSS另行报告。
共享内存核显上，不应把GPU显式字节数与进程RSS简单相加当成物理内存用量。

## 计时与算力口径

- 运算为`C=A×B`，有效计数为`2×M×N×K`，一次FMA／乘加按2个操作计。
- 每次执行一批输出行；只有一个kernel在途，等待完成后才提交下一批。
- GPU算力=`有效运算数 / GPU事件执行时间总和`。
- wall算力=`同样运算数 / 正式阶段主机墙钟时间`，包含提交、等待和进度输出开销。
- 浮点用GFLOPS，整数用GOPS；除以1000得到TFLOPS／TOPS。
- 编译、数据初始化与上传、校准／预热、最终读回校验都单独记录，不计入正式算力分母。
- 到达`-t`后等当前批次完成，因此实际时长可略超目标；报告保留实际时间。
- 自动批次目标约40 ms，但这是校准目标，不是硬实时或硬超时保证。

这是本项目tile GEMM实现的**实测性能**。内核分块、精度、矩阵大小和驱动都会影响结果，不等同理论峰值或厂商BLAS峰值。

### 正确性

每个精度先全量验证最多32×32矩阵；正式N≤64时全量验证输出，较大矩阵验证64个唯一单元，包含四角及确定性伪随机位置。
CPU独立标量循环生成参考，FP16每步按half RNE舍入，FP32／FP64使用对应FMA参考。
整数输入限制在[-3,3]，给定最大维度内不会发生累加溢出，要求精确相等。
浮点使用明确的绝对／相对容差，最大误差写入JSON。

程序对同用户／同后端设备索引使用非阻塞进程锁；重复测试进程会报busy。该锁不约束其他GPU应用或其他用户。

## 结果与常见问题

### 输出文件

终端默认表格按精度显示状态、N、GPU MiB、实际时间、设备算力、wall算力、单位与内核路径；每完成一项输出一行。
`passed`表示小矩阵自检和正式结果校验均通过；`probe`另以表格显示声明／编译／校验状态。
未支持的项目用`-`表示无成绩，不显示虚假的零算力。`-v`开启stderr详细进度，`-o`保存完整JSON。
每完成一个精度便原子更新报告，正常退出后`status=completed`。
JSON包括算术能力、auto或显式选择、每精度时长、矩阵尺寸、分配字节数、路径、计时、运算计数和校验信息。
不导出硬件身份或实际环境清单。验证方法见[`docs/validation/`](docs/validation/)，结果可用`tools/check_report.py`独立复核。

### 退出码

| 退出码 | 含义 |
|---|---|
| 0 | 至少一个精度通过，其他项可为正常unsupported |
| 1 | 后端／编译／分配／执行／校验或报告错误 |
| 2 | 参数、报告路径或已有输出文件错误 |
| 3 | 所请求的精度全部不支持 |
| 4 | GPU测试锁不可用或已有同用户测试进程 |
| 130 | SIGINT／SIGTERM中断，当前批次完成后保存部分结果 |

### `No OpenCL platform` / `-1001`

检查OpenCL loader、兼容运行时及render访问权，再运行`probe`。权限不足也可能表现为零个平台，而不是清晰的权限错误。
缺少访问权时可使用上文的临时render补充组命令。

### FP64被跳过

设备未声明FP64能力时，自动模式跳过并记录原因。
程序不会模拟FP64或转用CPU来生成GPU成绩。

### 分配失败或矩阵超预算

降低`-m`或`-n`。显式`-n`不满足预算时直接报错；运行时分配失败也会保留错误原因，不越过预算重试更大的分配。
设备声明可用内存不是操作系统当前空闲内存保证。

### 编译／校验失败

查看该精度的`reason`及`kernel_detail`。先运行`make check`和`make smoke`，仅在本地保留排查资料，不把环境诊断直接加入仓库。
某一声明支持的精度失败会使最终退出码非0，其他精度仍会按顺序记录结果。

### 中断和重跑

按Ctrl-C；程序在当前kernel完成后停止，保留已完成精度与部分计时结果。使用新的`-o`文件名重跑。
`tools/check_report.py`要求完整完成状态，因此会拒绝把中断报告当成完整成绩。

### 清理

```sh
make clean
```

测试不会安装服务、timer、cron或后台监测程序。真实运行数据保存在Git忽略的`results/`中，不随源码发布。

### 报告分享与隐私

默认报告只记录必要的算术能力、测试参数和测量数据，不记录主机名、账号、用户目录、网络地址、硬件身份、物理地址、驱动清单或执行日历时间。
默认构建不包含调试符号，并将编译工作目录映射为相对路径。需要调试信息时可显式设置`CFLAGS='-O0 -g'`。
发布报告、日志或补充文档前运行`make privacy-check`；凭据痕迹、个人邮箱、内网地址和身份元数据的检查范围见[隐私说明](docs/privacy.md)。
公开文档仅说明通用使用方法和测试口径，不描述实际设备环境或连接关系。
