# 基于 C++ 的本地多线程日志分析工具

项目简称 `netlog`。读取模拟设备运行日志，逐步实现设备指标统计、事件 Top-K 和按文件分配任务的多线程分析，作为个人工程学习项目。

**当前支持单文件和目录的串行或并行分析。** 程序逐行读取并校验日志，输出读入计数、设备指标和事件 Top-K，默认显示前 3 种事件。

## 日志格式

```text
timestamp device_id level event_code duration_us
```

实际数据示例（不含表头）：

```text
1700000000 dev01 INFO REQ_OK 100
1700000001 dev01 ERROR TIMEOUT 900
```

时间戳为非负整数秒，耗时为非负整数微秒；设备和事件标识只含英文字母、数字、下划线、连字符；日志级别为 `INFO/WARN/ERROR`。每行代表一次请求处理的最终结果。

完整约定见 [docs/log-format.md](docs/log-format.md)，包括数值范围、空行、换行符、重复记录和统计口径。

## 文件结构

```text
.
├── CMakeLists.txt          构建程序和测试
├── README.md
├── .gitignore
├── docs/log-format.md     日志协议与统计口径
├── include/netlog/
│   ├── record.h           日志级别与单条记录结构
│   ├── parser.h           解析接口、错误类型、结果类型
│   └── stats.h            设备与事件统计接口
├── src/
│   ├── parser.cpp         字段拆分、校验、类型转换
│   ├── main.cpp           参数处理、文件读取、诊断和报告输出
│   └── stats.cpp          累加统计并计算事件排行
├── data/sample.log        5 条合法记录和 1 条坏行
├── tests/
│   ├── test_parser.cpp    解析规则和数据所有权测试
│   └── test_cli.cmake     文件读取与命令行场景测试
└── build/                 本地构建生成，不提交
```

后续实现筛选时，再按职责拆分读取与报告模块。

## 构建与运行

依赖支持 C++17 的编译器和 CMake 3.16 或更新版本；没有第三方运行库依赖。下面的命令均在项目根目录执行。

当前主要开发环境为 **WSL2 / Ubuntu**，项目目录是：

```text
/home/lenovo/projects/netlog
```

Windows 桌面原目录保留为迁移前备份。后续开发请使用 WSL 中这份项目。

在 Ubuntu 终端中执行以下 Bash 命令：

```bash
cd ~/projects/netlog
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
./build/netlog --input ./data/sample.log
ctest --test-dir build --output-on-failure
```

运行解析器的独立测试：

```bash
./build/test_parser
```

如果是在一个全新的 Ubuntu 环境中，先安装依赖：

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake
```

本次迁移已安装 Linux 版 CMake，并在 WSL 内重新构建。Windows 产生的 `build/` 没有复制过来；两套环境应分别生成各自的构建产物。

从 Windows 终端进入项目的 Bash 会话：

```text
wsl -d Ubuntu --cd /home/lenovo/projects/netlog
```

从 Windows 文件资源管理器访问项目：

```text
\\wsl.localhost\Ubuntu\home\lenovo\projects\netlog
```

如果使用 VS Code，可以在 WSL 终端中执行 `code .`；需要安装微软的 WSL 扩展，并确认编辑器左下角显示连接到 WSL / Ubuntu。

路径包含空格时，加引号。当前 `--input` 接受一个普通文件或目录；`--help` 查看用法。

目录示例：

~~~bash
./build/netlog --input data/samples --threads 4 --top 5
~~~

程序按文件名顺序读取目录当前层的小写 .log 普通文件，跳过子目录与符号链接。默认用一个线程串行处理。`--threads N` 要求正整数，并行时，小文件作为一个任务，超过 16 MiB 的文件按字节切成多个区间，切点对齐到下一条日志的行首。实际并发数最多等于任务数。工作线程通过原子下标领取区间，各自统计设备指标和完整事件频次。主线程等待所有线程结束，再合并结果并计算全局 Top-K。并行模式按文件和区间顺序输出坏行诊断，通过前面各区间的读入行数恢复原始行号。诊断占用的内存随坏行数增加。分析期间输入文件应保持不变。每个文件的坏行从第 1 行起定位。目录中没有匹配文件或发生文件访问错误时，以退出码 3 结束。直接指定单个普通文件时允许任意扩展名。

`--top K` 接受正整数，默认为 3。不同事件不足 K 种时，输出所有事件。

样例的标准输出：

```text
read_lines=6
valid_records=5
invalid_records=1
devices:
device_id requests errors error_rate avg_us max_us
dev01 3 1 33.33% 400.00 900
dev02 2 1 50.00% 300.00 400
top_events (up to 3):
REQ_OK 3
CRC_ERROR 1
TIMEOUT 1
```

设备按 ID 升序排列。错误率为 `ERROR` 记录数除以该设备的有效记录数，平均耗时包含失败请求，耗时单位为微秒。事件按频次降序排列，同频按事件码字典序升序，最多显示 K 项。每台设备的总耗时超过 `uint64_t` 范围时，程序报错并以退出码 4 结束。

标准错误流另外报告第 6 行缺字段。两条输出流在终端的显示顺序不作保证。程序界面暂用英文，便于跨终端验证，说明文档和关键代码注释使用中文。

| 退出码 | 含义 |
|---|---|
| 0 | 完成读取，或显示帮助；存在坏行也允许正常完成 |
| 2 | 命令行参数错误 |
| 3 | 文件状态、打开或读取失败 |
| 4 | 非预期运行错误 |

## 为什么采用函数，而没有使用虚函数继承

当前只有一种固定日志格式，解析过程不需要保存跨行状态。把每种校验写成一个继承基类的对象，会增加类型、对象和调用关系，却没有解决当前的新问题。

因此采用“普通函数组合 + 明确的返回结果”：

```text
main.cpp：打开文件 → getline 读一行 → parse_line
                                           ↓
parser.cpp：split_fields → 数字/标识/级别校验
                                           ↓
                           Record 或 ParseError
                                           ↓
main.cpp：成功计数，或打印 文件名:行号:原因 并继续
```

`parser.cpp` 内部的辅助函数放在匿名命名空间，只供这个源文件使用：

| 函数 | 职责 |
|---|---|
| `split_fields` | 切分字段，检查恰好五个 |
| `is_identifier` | 检查设备和事件标识 |
| `parse_number` | 检查纯十进制数字，转换并检查类型范围 |
| `parse_level` | 将固定文本转换为 `Level` 枚举 |
| `parse_line` | 按顺序组织校验，成功后组装完整记录 |
| `error_message` | 将错误类型转换成诊断文本 |

公开接口是：

```cpp
using ParseResult = std::variant<Record, ParseError>;
ParseResult parse_line(std::string_view line);
```

`std::variant` 表示结果是两种类型之一。成功时里面是一条 `Record`，失败时里面是 `ParseError`，调用方通过 `std::get_if` 判断是哪一种。这样不会出现“返回失败，却让调用者误用半条记录”的情况，也不会把错误原因藏在打印语句中。

数字校验先确认每个字符都是数字，再用 `std::from_chars` 转换；同时检查返回错误码和是否消费了整个字段。这样能区分 `100abc`、`-1` 等格式错误与超出类型上限的范围错误。[C++ 工作草案中的转换语义](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2017/n4659.pdf#page=551)

`string_view` 只借用当前行做切片；返回记录里的设备 ID 和事件码使用 `std::string` 持有自己的数据，下一次 `getline` 覆盖缓冲区也不会破坏记录。

格式错误是预期会出现的数据问题，用返回值处理；文件打开和读取错误由 `main.cpp` 处理；最外层 `try/catch` 只兜底非预期的标准异常。解析器本身不打印、不访问文件、不修改共享状态，便于测试，也方便以后在线程中复用。

如果将来真的同时支持多种格式，并需要运行时切换解析器，再考虑统一接口、函数对象或带虚函数的 `Parser` 基类。若某种解析方式需要保存配置或状态，可以先用普通类封装；“使用类”和“必须使用继承”是两回事。

## 测试与下一步

多线程切分版在 Release 构建下通过了 63 项 CTest 测试。其中 `cli.segment_boundaries` 使用超过 16 MiB 的文件，覆盖切点落在 CRLF 之间、切点恰好在行首、末行无换行，以及切换文件后行号重新计数。测试用固定的预期报告和坏行提示核对 1、2、8 线程的输出。

测试覆盖合法字段值、数值边界与越界、非法字符、缺失/额外字段、级别大小写、BOM、嵌入空字符，以及记录是否持有自己的字符串。

命令行测试覆盖样例、空文件、CRLF、无末尾换行、路径含空格、跳过坏行后继续读取、全部为坏行、文件不存在、目录输入和错误参数。测试不依赖 `assert`，Release 构建仍执行检查。

逐行读取不会保存整个文件，内存主要随最长一行变化。当前未设置行长度上限，每条坏行都会打印诊断；这是本地学习版本，后续处理大规模数据时可以增加长度限制与诊断限额。读权限不足和读到一半出现 I/O 故障的处理分支已写入，但不属于当前自动化测试覆盖范围。

建议阅读顺序：`record.h` → `parser.h` → `parser.cpp` → `main.cpp` → 测试。下一步支持筛选。

## 手工样例与大数据生成

`data/sample.log` 是基础演示。`data/samples/` 里的小样例可以直接用 `./build/netlog --input 文件路径` 运行，并按下表核对。

| 文件 | 预期结果 |
|---|---|
| `aggregation.log` | 有效 5 行。`dev-a` 有 3 次请求、0 次错误、平均 133.67 µs、最大 201 µs。`dev-z` 有 2 次请求、1 次错误、平均 600.00 µs、最大 900 µs。重复记录照常计数，`REQ_OK` 共 3 次。 |
| `top-k.log` | 有效 5 行。`Z_EVENT` 2 次居首，`A_EVENT` 和 `B_EVENT` 各 1 次排在其后。同频的 `C_EVENT` 超出前三。 |
| `mixed.log` | 共 6 行，有效 2 行，非法 4 行。第 2～5 行分别是负耗时、非法级别、非法设备 ID、字段数错误。`dev01` 错误率 50%，平均 250.00 µs。 |

使用 Python 3 生成可复现的模拟日志。在项目根目录执行：

```bash
python3 scripts/generate_logs.py data/generated/large.log --lines 1000000 --devices 1000 --seed 42
./build/netlog --input data/generated/large.log
```

相同的参数和 Python 环境会产生相同的日志。生成器逐行写入，已存在的输出文件会保留。`--devices` 指可抽取的设备 ID 数量。使用 `--bad-every 500` 可以固定每 500 行放入一条坏行，`--lines` 包含这些坏行。`data/generated/` 已被 Git 忽略。

## 多线程性能实测

在 WSL2 / Ubuntu、Intel Core i9-12900H（20 个逻辑 CPU）上，用 GCC 13.3.0 构建 Release 版。两档输入各有 8 个等长文件，约 35 MB 和 350 MB。每种线程数预热一次，交替运行 5 轮取墙钟耗时的中位数。加速比以同一构建、同一输入的 1 线程结果为基准。

| 线程数 | 100 万行中位耗时 | 加速比 | 1000 万行中位耗时 | 加速比 |
|---:|---:|---:|---:|---:|
| 1 | 0.1615 s | 1.00× | 1.5698 s | 1.00× |
| 2 | 0.0837 s | 1.93× | 0.8189 s | 1.92× |
| 4 | 0.0467 s | 3.46× | 0.4331 s | 3.62× |
| 8 | 0.0340 s | 4.75× | 0.2897 s | 5.42× |

各线程数的完整标准输出均与串行基线逐字节一致。另一次手动复测中，1000 万行的 1 线程和 8 线程分别跑了 1.771 s 和 0.327 s，约为 5.42×。对比用 `time` 输出里的 `real`。`user` 和 `sys` 分别是所有线程累计的用户态和内核态 CPU 时间。以上结果对应这台机器上的预热输入，此表的输入是 8 个等长文件。

在 Ubuntu / WSL 的 Bash 中，可以用固定种子重新生成两档数据。百万行数据的每个分片在千万行数据中重复 10 次，因此两档的设备和事件分布相同。

~~~bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel

mkdir -p data/generated
bench=$(mktemp -d data/generated/bench-XXXXXX)
mkdir -p "$bench/1m" "$bench/10m"
python3 scripts/generate_logs.py "$bench/source.log" \
  --lines 1000000 --devices 1000 --seed 42
split -l 125000 -d --additional-suffix=.log "$bench/source.log" "$bench/1m/part-"

for part in "$bench"/1m/*.log; do
  for repeat in {1..10}; do
    cat "$part"
  done > "$bench/10m/${part##*/}"
done
~~~

先预热，再逐次查看 `real`。换成 `dataset="$bench/1m"` 可以跑百万行档。

~~~bash
dataset="$bench/10m"
for threads in 1 2 4 8; do
  ./build-release/netlog --input "$dataset" --threads "$threads" > /dev/null
done
for round in {1..5}; do
  for threads in 1 2 4 8; do
    echo "round=$round threads=$threads"
    time ./build-release/netlog --input "$dataset" --threads "$threads" > /dev/null
  done
done
~~~

### 超大单文件与大小悬殊的目录

超过 16 MiB 的日志文件按字节切分，切点对齐到行首。小文件各占一个任务。线程记录片段内的坏行行号，主线程合并时按文件及片段顺序加上前面片段的总行数，得到原始文件行号。

在同一台 WSL2 机器用 GCC 13.3 构建 Release。对比基线按整文件分配任务，切分版按片段分配任务。单个大文件有 99,999,900 行，加上 100 行的小文件后共 1 亿行，约 3.5 GB。读入预热后，单文件每种配置计时 1 次，混合目录每种配置交替计时 3 次，表中取中位数。

| 输入 | 线程数 | 切分前（秒） | 切分后（秒） | 加速比 |
|---|---:|---:|---:|---:|
| 单个大文件 | 1 | 16.5775 | 16.3541 | 1.01× |
| 单个大文件 | 8 | 16.6913 | 2.5658 | 6.51× |
| 大文件加小文件 | 1 | 16.2136 | 16.1352 | 1.00× |
| 大文件加小文件 | 8 | 15.5225 | 2.7173 | 5.71× |

两版在 1 和 8 线程下的标准输出和坏行提示分别通过 SHA-256 一致性核对。此前千万行、8 个等长文件的输入也用 8 线程复测了 5 轮，中位耗时从 0.2918 s 变为 0.2997 s，主要收益出现在原本由一个大文件拖慢的输入。

本机完整报告和原始计时保存在 `data/generated/segment-skew-100m/comparison.md` 及同目录的 `before.json`、`after.json`。该目录是本地生成数据，已被 Git 忽略。保留样例后可在项目根目录复跑。

~~~bash
cmake -S . -B build-release-segments -DCMAKE_BUILD_TYPE=Release
cmake --build build-release-segments --parallel
time ./build-release-segments/netlog \
  --input data/generated/segment-skew-100m/mixed --threads 8 > /dev/null
~~~

## 保存测试报告

CTest 分别运行解析用例、统计用例和命令行场景。测试代码在 `tests/`，手工样例在
`data/samples/`。运行生成的报告放在已被 Git 忽略的 `build/test-results/`。

```bash
cmake --build build --parallel
mkdir -p build/test-results
stamp=$(date +%Y%m%d-%H%M%S)
ctest --test-dir build --output-on-failure \
  --output-junit "test-results/$stamp.xml" \
  --output-log "build/test-results/$stamp.log"
```

XML 使用 JUnit 格式，记录每个测试名、结果和耗时，需要 CTest 3.21 或更新版本。
文本日志记录每项用例的运行输出及耗时。单个测试的耗时包含启动和测试框架开销，
大文件吞吐量的实验应单独测量。
