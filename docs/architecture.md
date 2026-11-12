# 后端与平台扩展设计

## 边界

`include/gx_backend.h`是后端中立的C ABI，核心层不引用OpenCL、Metal、CUDA、HIP类型。

```text
main / config / model / report / runner
                    |
               gx_backend_ops
                    |
             OpenCL（当前实现）
             Metal / CUDA / HIP（未来注册）
```

- `config.c`负责短参数、时长覆盖和预算反推，不判断GPU厂商。
- `model.c`定义精度、输入／累加类型、半精度转换和CPU参考结果。
- `runner.c`负责串行调度、逐精度生命周期、校准、计时、结果校验。
- `backend_opencl.c`独立承担平台枚举、特性查询、编译和设备内存／事件管理。
- `kernels/gemm.cl`用类型宏选择内核，无Intel设备ID白名单。
- `os.c`集中实现POSIX时钟、信号、RSS、同用户设备锁；平台专属逻辑集中于此。

## 后端契约

1. `open`只选择GPU，填充通用能力结构；不隐式改用CPU。
2. `compile`返回内核和明确的精度／累加路径，按可选能力选择加速实现。
3. `create`检查所有显式GPU数据缓冲区之和不超过预算；失败必须释放已创建对象。
4. `initialize`生成确定性、非全零输入，准备成本不计入正式测试。
5. `execute`同步完成单批，返回设备执行纳秒；同一时刻最多一个测试kernel在途。
6. `read`仅用于计时外校验；不能偷换数据或CPU计算结果。
7. `destroy`在设备工作完成后释放本精度所有数据，随后才允许下一精度分配。
8. `release_kernel`和`close`释放编译资源、队列及上下文。

未来异步API可在后端内部提交后等待，但对当前runner保持单批同步契约。跨进程锁防止同用户同时测试同一后端设备索引；它不是全系统GPU占用锁。

## 平台路径

| 平台／厂商 | 当前结构支持 | 验证状态 |
|---|---|---|
| OpenCL GPU | 运行时查询、内核编译和数值自检 | 不公开实际设备清单；验证方法见validation |
| AMD GPU | 通用OpenCL枚举、类型检测、portable kernel | 尚未真机验证；可增加HIP后端 |
| NVIDIA GPU | 通用OpenCL枚举、类型检测、portable kernel | 尚未真机验证；可增加CUDA后端 |
| macOS ARM64 | Makefile的OpenCL framework路径、Apple头文件、POSIX/macOS RSS适配 | 尚未真机验证；推荐未来增加Metal后端 |

Apple的OpenCL接口已弃用且能力受驱动限制；源码中的构建适配不等于已验证Apple GPU运行支持。
AMD／NVIDIA的OpenCL路径不利用尚未实现的MFMA、WMMA、Tensor Core等厂商专用矩阵功能。

## 新增后端步骤

1. 新建独立翻译单元，如`src/backend_metal.m`或`src/backend_cuda.c`，实现`gx_backend_ops`。
2. 将API对象封装在私有opaque状态中，通过C ABI接入；Objective-C/C++桥接放在该后端内部。
3. 在`src/backend.c`注册，在Makefile增加对应源文件／链接规则和编译工具。
4. 复用核心配置、预算规划、参考校验和JSON schema；有额外工作区时计入预算。
5. 用相同输入、相同输入／累加类型、相同`2*M*N*K`计数比较结果。
6. 新增真机验证记录后，才能将平台标为verified。

## 精度语义

INT8与INT16是窄输入、INT32累加的GEMM。通用实现明确转换到INT32；INT8 dot实现明确使用扩展点积。
不依据驱动属性单独宣称原生指令峰值；公开报告仅保留必要的算术能力与测量数据。
FP16使用half输入与half FMA累加；不会把FP32运算伪装成FP16算力。
数值参考为CPU独立标量循环，FP16逐步进行RNE半精度舍入；输入范围使整数累加不会溢出。

## 内核与批次

默认16×16输出tile、8×8 work-items，每个work-item维护4个累加器。边界加载填0，输出带边界检查，因此支持`N=37`等非整tile尺寸。
先校准一个小行批，再把批次目标设为约40 ms，最多512行。单批最小为16行（小矩阵可更少），无法承诺任意设备上精确40 ms。
`-t`是主机正式阶段的墙钟目标；达到目标后完成当前批次，记录实际时长。

自动矩阵尺寸上限为4096，按tile向下对齐；`-m`是上限，不要求用满。`-n`可以显式指定更大矩阵，但仍检查预算、单次分配上限和尺寸上限32768。
