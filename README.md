# 基于 C++ 的本地多线程日志分析工具

项目简称 `netlog`。读取模拟设备运行日志，逐步实现设备指标统计、事件 Top-K 和按文件分配任务的多线程分析，作为个人工程学习项目。

**当前阶段为单文件串行分析。** 程序逐行读取并校验日志，输出读入计数、设备指标和事件 Top-3。

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

后续实现多文件和筛选时，再按职责拆分读取与报告模块。

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

路径包含空格时，加引号。当前 `--input` 接受一个普通文件，不接受目录；`--help` 查看用法。

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

设备按 ID 升序排列。错误率为 `ERROR` 记录数除以该设备的有效记录数，平均耗时包含失败请求，耗时单位为微秒。事件按频次降序排列，同频按事件码字典序升序，最多显示三项。每台设备的总耗时超过 `uint64_t` 范围时，程序报错并以退出码 4 结束。

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

测试覆盖合法字段值、数值边界与越界、非法字符、缺失/额外字段、级别大小写、BOM、嵌入空字符，以及记录是否持有自己的字符串。

命令行测试覆盖样例、空文件、CRLF、无末尾换行、路径含空格、跳过坏行后继续读取、全部为坏行、文件不存在、目录输入和错误参数。测试不依赖 `assert`，Release 构建仍执行检查。

逐行读取不会保存整个文件，内存主要随最长一行变化。当前未设置行长度上限，每条坏行都会打印诊断；这是本地学习版本，后续处理大规模数据时可以增加长度限制与诊断限额。读权限不足和读到一半出现 I/O 故障的处理分支已写入，但不属于当前自动化测试覆盖范围。

建议阅读顺序：`record.h` → `parser.h` → `parser.cpp` → `main.cpp` → 测试。下一步支持多文件串行分析、筛选和可配置的 Top-K。
