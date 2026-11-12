# 验证方法

公开仓库仅说明验证方法，不发布真实运行数据、设备身份、环境清单或连接关系。
运行报告与日志保留在Git忽略的`results/`目录。

## 核心检查

```sh
make check
make privacy-check
```

核心检查覆盖参数、预算反推、精度语义、半精度转换、有效运算计数、表格输出及报告元数据最小化。

## GPU正确性

```sh
mkdir -p results
./build/graphics-xps probe -pauto -o results/probe.json
./build/graphics-xps bench -pauto -n37 -m16M -t0.05 -w0 -o results/boundary.json
./build/graphics-xps bench -pauto -n256 -m32M -t0.1 -w0 -o results/sampled.json
```

- 自动查询能力、编译可用路径，先全量核对小矩阵。
- `N=37`检查非整tile的行列及K维尾部。
- 较大矩阵检查确定性采样读回，包含边界与非对角元素。
- 没有声明所需能力的精度跳过；声明支持但编译／计算失败应报错。
- 测试项目严格串行，上一项目完成并释放数据后才开始下一项目。

## 配置正式测试

```sh
./build/graphics-xps bench -pauto -t10 -m1G -o results/benchmark.json
python3 tools/check_report.py results/benchmark.json
python3 tools/check_privacy.py --files results/benchmark.json
```

检查预算、矩阵字节数、有效运算计数、实际时长、派生算力和校验单元数。
本地结果应按使用者的数据处理要求保存，不自动进入公开仓库。

## 理论峰值与GEMM实测

理论峰值通常为：

```text
计算单元数量 × 频率 × 每单元每周期的有效运算数
```

该值要求对应指令路径、频率和持续发射利用率。当前通用tile GEMM包含局部内存访问、同步屏障、地址计算和边界处理，尚未采用厂商级调优，实测结果不代表硬件峰值。
低精度没有体现预期吞吐优势时，需要检查指令生成、数据复用和实际频率；不能仅凭成绩断言精度不受支持。
性能计时已排除初始化、上传和校验。评估接近峰值的计算能力，应另行使用寄存器FMA微基准或优化GEMM。
