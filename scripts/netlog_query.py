"""校验中文查询计划，预览命令或调用本地 C++ netlog。"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path


QUERY_PROMPT = """
你为本地 netlog 翻译一个独立的中文问题。只返回一个 JSON 对象。
你看不到日志，不推测结果、路径或设备。输入文件或整个目录已由调用方选好。
默认扫描全部设备、全部时间，事件计数与时间窗口天然可以跨文件、跨设备汇总。
用户说“整个目录”“所有设备合起来”时省略 --device。没有给设备或时间也可以查询。
成功格式如下，args 中选项和值都是字符串：
{"args":["--top","4"],"rank_by":null,"top_in_peak":null}

可用的查询参数只有以下五个。设备 ID 区分大小写，只能包含 ASCII 字母、数字、_ 和 -。
--device ID 精确匹配；--since S 包含起点；--until U 排除终点。S、U 为非负整数 Unix 秒。
--bucket N 按固定 N 秒汇总请求数、错误数、错误率、平均和最大耗时。
--top K 列出整个查询范围内出现次数最多的 K 种事件，统计全部级别。
N、K 为正整数。分钟乘 60，小时乘 3600。没指定的筛选项就省略。
输入路径和线程由本地控制，不在 args 里。单纯按分钟展示时 rank_by 为 null。

rank_by 表示“从多个时间窗口挑一个最高的”，不表示“要看哪一列”。
先确定答案的对象：若要返回一个时间段，可以给所有设备合计后比较窗口，
也可以固定一台已知设备比较它的窗口。若要返回设备名，则不能用窗口峰值代答。
devices 表按整个查询范围汇总每台设备；time_windows 表把匹配设备合计到各窗口。
两张表都没有“设备 × 窗口”的明细。比如“按分钟分组，报错最多的是哪台设备”
或“每分钟给出出错最多的设备”都返回 {"unsupported":"当前只能按设备看全时段汇总，或按窗口看设备合计，暂不支持按窗口给设备排名。"}。
先判断有没有挑选峰值：“逐个窗口列出失败率”只需要 --bucket，rank_by 必须为 null；
只有明确问哪个窗口出错次数最多才填 "errors"，问哪个窗口失败占比最高才填 "error_rate"。
例如，“每十秒看看失败比例”的输出为 {"args":["--bucket","10"],"rank_by":null,"top_in_peak":null}；
“找失败比例最高的十秒窗口”的输出为 {"args":["--bucket","10"],"rank_by":"error_rate","top_in_peak":null}。
rank_by 需要 --bucket；没给窗口大小就追问。其他查询 rank_by 填 null。
“整个时段前两种事件”放 --top 2，top_in_peak 为 null。
“峰值那一分钟的前两种事件”填 top_in_peak: 2，第一次查询不加 --top。
若只问峰值时段有哪些高频事件且没给数量，用默认 3；其余情况 top_in_peak 为 null。
没有时间限制就不加时间参数；左闭右开区间 [S,U) 原样使用。
如果明确说“包含第 U 秒”，等价于 --until U+1。区间为空、上下界颠倒、
“最严重”没说明指标，或相对时间缺少参考点时，返回 {"clarify":"一个具体的澄清问题"}。
请求无法用以上能力完成（如 P99、只统计 ERROR 级别的事件排名），
返回 {"unsupported":"简短原因"}。不要悄悄删掉用户要求的条件。
"""

FLAGS = ("--device", "--since", "--until", "--bucket", "--top")
MAX_SECOND = 2**63 - 1
MAX_COUNT = sys.maxsize * 2 + 1


def checked_args(args):
    if not isinstance(args, list) or len(args) % 2:
        raise ValueError("查询计划中的参数必须是选项和值的配对。")
    options = {}
    for flag, value in zip(args[::2], args[1::2]):
        if flag not in FLAGS or flag in options or not isinstance(value, str):
            raise ValueError("查询计划含有未知、重复或格式错误的参数。")
        if flag == "--device":
            if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
                raise ValueError("查询计划中的设备 ID 格式有误。")
        else:
            if not re.fullmatch(r"[0-9]+", value):
                raise ValueError(f"{flag} 需要一个十进制整数。")
            number = int(value)
            minimum = 1 if flag in ("--bucket", "--top") else 0
            maximum = MAX_COUNT if flag == "--top" else MAX_SECOND
            if not minimum <= number <= maximum:
                raise ValueError(f"{flag} 的值超出了可用范围。")
            value = str(number)
        options[flag] = value
    if "--since" in options and "--until" in options and int(options["--since"]) >= int(options["--until"]):
        raise ValueError("查询起点必须早于结束时间。")
    return options


@dataclass
class Query:
    options: dict
    rank: str | None
    top: int | None


def parse_plan(reply):
    try:
        plan = json.loads(reply)
    except ValueError as error:
        raise ValueError("模型返回的查询计划不是有效 JSON，请重新提问。") from error
    if not isinstance(plan, dict):
        raise ValueError("查询计划必须是 JSON 对象。")
    for field, prefix in (("clarify", "请补全问题后重问"), ("unsupported", "暂不支持")):
        if set(plan) == {field}:
            message = plan[field]
            if not isinstance(message, str) or not message.strip() or not message.isprintable():
                raise ValueError("模型返回的说明格式有误。")
            return f"{prefix}：{message}"
    if set(plan) != {"args", "rank_by", "top_in_peak"}:
        raise ValueError("查询计划的字段不完整或含有未知字段。")

    options = checked_args(plan["args"])
    rank = plan["rank_by"]
    top = plan["top_in_peak"]
    if rank not in (None, "errors", "error_rate") or (rank is not None and "--bucket" not in options):
        raise ValueError("峰值查询需要有效的窗口和比较指标。")
    if top is not None and (type(top) is not int or not 1 <= top <= MAX_COUNT or rank is None):
        raise ValueError("峰值事件排行需要先选定峰值窗口，并指定正整数 K。")
    return Query(options, rank, top)


def command(program, input_path, options):
    argv = [str(program), "--input", str(input_path)]
    for flag in FLAGS:
        if flag in options:
            argv.extend((flag, options[flag]))
    return argv


def render_plan(reply, input_path, program="./build/netlog"):
    query = parse_plan(reply)
    if isinstance(query, str):
        return query
    options, rank, top = query.options, query.rank, query.top
    lines = [shlex.join(command(program, input_path, options))]
    if rank is not None:
        metric = "错误数（errors）" if rank == "errors" else "错误率（errors/requests）"
        lines.append(f"# 比较 time_windows 的{metric}，并列取最早的窗口。无匹配则结束。")
    if top is not None:
        second = command(program, input_path, {})
        if "--device" in options:
            second.extend(("--device", options["--device"]))
        lines.append("# PEAK_SINCE 和 PEAK_UNTIL 填所选窗口与原查询范围交集的起点和终点。")
        lines.append(shlex.join(second) + ' --since "${PEAK_SINCE:?}" --until "${PEAK_UNTIL:?}" --top ' + str(top))
    return "\n".join(lines)


def run_netlog(argv):
    print("$ " + shlex.join(argv), flush=True)
    try:
        # stderr 直接交给终端，坏行多时也不在 Python 中积攒诊断。
        run = subprocess.run(argv, stdout=subprocess.PIPE, encoding="utf-8", check=True)
    except subprocess.CalledProcessError as error:
        raise RuntimeError(f"netlog 以状态码 {error.returncode} 结束，查询中止。") from error
    except OSError as error:
        raise RuntimeError(f"无法启动 netlog：{error}") from error
    return run.stdout


def peak_window(report, width, rank):
    header = (f"time_windows ({width} seconds each):\n"
              "start_ts requests errors error_rate avg_us max_us\n")
    _, found_header, remaining = report.partition(header)
    rows, found_events, _ = remaining.partition("top_events (up to ")
    if not found_header or not found_events:
        raise ValueError("netlog 报告缺少时间窗口或事件表，无法判断峰值。")

    best_key = None
    best = None
    for row in rows.splitlines():
        start, requests, errors, _, _, _ = row.split()
        start, requests, errors = int(start), int(requests), int(errors)
        score = Fraction(errors, requests) if rank == "error_rate" else errors
        key = (score, -start)
        if best_key is None or key > best_key:
            best_key = key
            best = (start, requests, errors)
    return best


def event_table(report, limit):
    header = f"top_events (up to {limit}):\n"
    _, found, rows = report.partition(header)
    if not found:
        raise ValueError("netlog 报告缺少预期的事件排行。")
    return (header + rows).rstrip()


def execute_plan(reply, input_path, program):
    query = parse_plan(reply)
    if isinstance(query, str):
        return query
    options = query.options
    report = run_netlog(command(program, input_path, options))
    if query.rank is None:
        return report.rstrip()

    width = int(options["--bucket"])
    peak = peak_window(report, width, query.rank)
    if peak is None:
        return "没有匹配记录，无法选出峰值窗口。"
    start, requests, errors = peak
    since = max(start, int(options.get("--since", 0)))
    until = min(start + width, int(options.get("--until", MAX_SECOND + 1)))
    metric = "错误率" if query.rank == "error_rate" else "错误数"
    result = [f"{metric}最高的窗口（有效范围）[{since}, {until})："
              f"{requests} 次请求，{errors} 次错误，错误率 {100 * errors / requests:.2f}%。"]
    if "--top" in options:
        result.append("整个查询时段的事件排行\n" + event_table(report, options["--top"]))
    if query.top is not None:
        narrow = {"--since": str(since), "--top": str(query.top)}
        if "--device" in options:
            narrow["--device"] = options["--device"]
        # 超过 int64 最大时间戳的右端点等价于无上界。
        if until <= MAX_SECOND:
            narrow["--until"] = str(until)
        second = run_netlog(command(program, input_path, narrow))
        result.append("峰值窗口的事件排行\n" + event_table(second, query.top))
    return "\n".join(result)


def query_session(input_path=None, *, preview=False):
    from deepseek_toy import ask_deepseek, interactive_session

    root = Path(__file__).resolve().parents[1]
    in_project = Path.cwd() == root
    program = "./build/netlog" if in_project else str(root / "build/netlog")
    default_file = Path("data/sample.log") if in_project else root / "data/sample.log"
    path = Path(input_path).expanduser() if input_path is not None else default_file
    if not preview:
        if not (path.is_file() or path.is_dir()):
            print(f"输入文件或目录不存在或不可用：{path}", file=sys.stderr)
            return 2
        if not (Path(program).is_file() and os.access(program, os.X_OK)):
            print(f"找不到可执行的 netlog：{program}。请在项目根目录构建 build/netlog。", file=sys.stderr)
            return 2

    action = render_plan if preview else execute_plan

    def answer(question, *, api_key):
        reply = ask_deepseek(question, api_key=api_key, system_prompt=QUERY_PROMPT, json_output=True)
        return action(reply, path, program)

    label = "netlog 命令预览" if preview else "netlog 本地查询"
    return interactive_session(f"{label} · {path}", answer)


def main():
    parser = argparse.ArgumentParser(description="用自然语言查询本地日志")
    parser.add_argument("--input", metavar="路径", help="文件或目录，默认使用项目内的 data/sample.log")
    parser.add_argument("--preview", action="store_true", help="只预览命令")
    args = parser.parse_args()
    if args.input == "":
        parser.error("--input 需要非空路径")
    return query_session(args.input, preview=args.preview)


if __name__ == "__main__":
    sys.exit(main())
