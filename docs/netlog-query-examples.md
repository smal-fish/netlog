# 用中文查询本地日志

在 WSL 项目根目录启动。每次输入一个完整问题，输入 `exit` 退出。

```bash
cd ~/projects/netlog
python3 scripts/netlog_query.py
```

查询入口默认读取项目内的 `data/sample.log`。使用 `--input` 选自己的文件或目录，显式给出的相对路径从启动命令的当前目录解析；目录沿用 C++ 程序的规则，处理当前层的 `.log` 文件。输入路径不可用时直接报告该路径。程序需要已构建的 `build/netlog` 和 `DEEPSEEK_API_KEY`。

```bash
python3 scripts/netlog_query.py --input "data/incident/part-a.log"
python3 scripts/netlog_query.py --input data/incident
python3 scripts/netlog_query.py --input data/incident --preview
```

DeepSeek 每次只接收当前问题与固定的查询规则。Python 校验返回的参数并显示实际调用的命令，C++ 在本地读取所选输入并计算统计量。普通查询直接输出 C++ 报告。峰值查询按错误次数或精确分数（错误数 / 请求数）选窗口；并列取最早，无匹配则停止。只有用户还问峰值窗口内的事件时，才把窗口与原查询范围取交集，再运行一次 C++。分析期间应保持输入不变；同一坏行可能在两次扫描时各报告一次。

例如在 `data/incident` 上问“在 [120,300) 内，dev01 哪一分钟错误率最高？列出那一分钟最常见的两种事件。”实际查询会给出 `[180,240)`、4 次请求、3 次错误、75.00%，以及 `TIMEOUT 2` 和 `CRC_ERROR 1`。选择 `--preview` 则只显示命令。旧的 `deepseek_toy.py` 仍可用于普通问答，其 `--input` 入口保留命令预览。

## 实测输入与输出

2026-11-23 使用 `deepseek-flash` 对合成问句做了两轮真实调用。第一轮 14 个问题有 11 个符合预期。三个偏差来自两个理解问题：模型误以为必须指定某台设备，以及把逐窗口展示错误率理解成要挑选峰值。提示词补充了默认的设备范围，并给出了“列出窗口”和“挑出最高窗口”的成对示例。第二轮重跑原题并加入三个新问法，以下 17 个结果均与预期语义一致。数据是此次实测记录，模型再次调用时可能有差异。

### 一条命令即可预览的查询

| 问题 | 终端显示的命令 |
|---|---|
| 整个目录里出现次数最多的 5 种事件是什么？ | `./build/netlog --input data/incident --top 5` |
| 给我看 dev01 的请求数、平均耗时和最大耗时。 | `./build/netlog --input data/incident --device dev01` |
| 查 dev01 在 [120,300) 内最常见的两个事件。 | `./build/netlog --input data/incident --device dev01 --since 120 --until 300 --top 2` |
| 看 dev01 在 [120,300) 每一分钟的错误率，所有窗口都列出来。 | `./build/netlog --input data/incident --device dev01 --since 120 --until 300 --bucket 60` |
| 从第 180 秒开始（含），查看 dev01 的前四种高频事件。 | `./build/netlog --input data/incident --device dev01 --since 180 --top 4` |
| 只统计第 180 秒到第 239 秒，两个端点都包含，列出前三种常见事件。 | `./build/netlog --input data/incident --since 180 --until 240 --top 3` |
| 整份日志按小时列出请求数和平均耗时，每个小时都要。 | `./build/netlog --input data/incident --bucket 3600` |
| 只看设备 Rack_A-7，在第 900 秒之前找最常见的 8 种事件。 | `./build/netlog --input data/incident --device Rack_A-7 --until 900 --top 8` |

### 涉及峰值的查询

只找峰值时，一条命令列出所有非空窗口，旁边的注释说明比较哪一列。查峰值窗口内的高频事件时，要再查一次；第二条模板的两个变量需要根据第一条命令的结果填写。时间范围取所选窗口与原始筛选条件的交集。

**dev01 在哪五分钟的固定窗口里报错条数最多？**

```bash
./build/netlog --input data/incident --device dev01 --bucket 300
# 比较 time_windows 的错误数（errors），并列取最早的窗口。无匹配则结束。
```

**所有设备合在一起，每分钟算一次，哪分钟失败请求的占比最高？**

```bash
./build/netlog --input data/incident --bucket 60
# 比较 time_windows 的错误率（errors/requests），并列取最早的窗口。无匹配则结束。
```

**在 [120,300) 内，dev01 哪一分钟失败率最高？再查那一分钟出现最多的两种事件。**

```bash
./build/netlog --input data/incident --device dev01 --since 120 --until 300 --bucket 60
# 比较 time_windows 的错误率（errors/requests），并列取最早的窗口。无匹配则结束。
# PEAK_SINCE 和 PEAK_UNTIL 填所选窗口与原查询范围交集的起点和终点。
./build/netlog --input data/incident --device dev01 --since "${PEAK_SINCE:?}" --until "${PEAK_UNTIL:?}" --top 2
```

在 `data/incident` 上实际取到的边界代入后，第二条命令如下。

```bash
./build/netlog --input data/incident --device dev01 --since 180 --until 240 --top 2
```

**只看 dev01 的 [185,225)，按一分钟分桶，找错误条数最多的窗口，并列出该窗口前三种常见事件。**

```bash
./build/netlog --input data/incident --device dev01 --since 185 --until 225 --bucket 60
# 比较 time_windows 的错误数（errors），并列取最早的窗口。无匹配则结束。
# PEAK_SINCE 和 PEAK_UNTIL 填所选窗口与原查询范围交集的起点和终点。
./build/netlog --input data/incident --device dev01 --since "${PEAK_SINCE:?}" --until "${PEAK_UNTIL:?}" --top 3
```

在 `data/incident` 上实际取到的边界代入后，第二条命令如下。

```bash
./build/netlog --input data/incident --device dev01 --since 185 --until 225 --top 3
```

**从 Unix 秒 600 起，跨设备按 10 秒分段，找 ERROR 次数最大的那段。**

```bash
./build/netlog --input data/incident --since 600 --bucket 10
# 比较 time_windows 的错误数（errors），并列取最早的窗口。无匹配则结束。
```

### 需要澄清或当前能力不支持的查询

| 问题 | 本轮实际回复 |
|---|---|
| dev01 哪一分钟最严重？ | 请补全问题后重问：“最严重”是指哪个指标：错误次数最多，还是失败占比最高？ |
| 哪个固定时间窗口的错误率最高？ | 请补全问题后重问：请指定固定时间窗口的大小（例如每 10 秒、每 1 分钟） |
| 统计 dev01 请求耗时的 P99。 | 暂不支持：当前能力不支持 P99 统计，只有平均值和最大值 |
| 只在 ERROR 级别的记录中选出最常见的两个事件。 | 暂不支持：仅按 ERROR 级别筛选并排名事件超出可用能力，--top 不按级别过滤。 |

校验时先手写每题的预期参数、比较指标及峰值窗口内的 K，再与模型输出逐项比较。对可生成命令的 13 题，另外在本地调用了已有的 C++ 程序验证命令；两条含第二次查询的样例也根据本地窗口计数算出了实际边界再运行。上面的命令输出记录来自预览模式。[DeepSeek JSON Output 手册](https://api-docs.deepseek.com/zh-cn/guides/json_mode/)
