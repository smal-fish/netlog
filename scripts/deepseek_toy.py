"""在终端提问，调用 DeepSeek 获取简短的中文解释。"""

import argparse
import json
import os
import sys
from contextlib import contextmanager
from http.client import HTTPException
from urllib.error import HTTPError
from urllib.request import Request, urlopen


API_URL = "https://api.deepseek.com/chat/completions"
MODEL = "deepseek-flash"
TEACHING_PROMPT = "你是一位计算机基础课助教。请用中文回答，先用一句话解释，再给一个具体的小例子。"
INPUT_ENCODING_ERROR = "这行输入含有无法按 UTF-8 读取的字符，请重新输入，或按 Ctrl+C 退出。"


def ask_deepseek(question, *, api_key, system_prompt=TEACHING_PROMPT, json_output=False):
    """发送一轮独立的问题，返回模型答复的文本。"""
    payload = {
        "model": MODEL,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": question},
        ],
        "thinking": {"type": "disabled"},
        "stream": False,
        "max_tokens": 512,
    }
    if json_output:
        payload["response_format"] = {"type": "json_object"}
    try:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    except UnicodeEncodeError as error:
        raise RuntimeError(INPUT_ENCODING_ERROR) from error
    request = Request(
        API_URL,
        data=body,
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )
    try:
        with urlopen(request, timeout=60) as response:
            reply = json.load(response)
    except HTTPError as error:
        hints = {
            401: "请重新设置 DEEPSEEK_API_KEY 并重启程序。",
            402: "请到 DeepSeek 开放平台检查 API 账户余额。",
            429: "请求过于频繁，请稍后再试。",
            503: "服务繁忙，请稍后再试。",
        }
        hint = hints.get(error.code, "请根据状态码查看 DeepSeek 官方错误码手册。")
        raise RuntimeError(f"DeepSeek 返回 HTTP {error.code}。{hint}") from error
    except (OSError, HTTPException) as error:
        raise RuntimeError("连接 DeepSeek 失败或超时，请检查 WSL 网络后再试。") from error
    except ValueError as error:
        raise RuntimeError("DeepSeek 返回的内容不是有效 JSON。") from error

    try:
        text = reply["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError) as error:
        raise RuntimeError("DeepSeek 的响应中缺少回答字段。") from error
    if not isinstance(text, str) or not text.strip():
        raise RuntimeError("DeepSeek 返回了空回答，请重新提问。")
    return text.strip()


def is_exit_command(line):
    # 终端退格可能留下半个 UTF-8 字符。仅为识别命令容忍首尾坏字节。
    raw = line.encode("utf-8", errors="surrogateescape")
    if raw.strip() == "退出".encode("gb18030"):
        return True
    command = raw.decode("utf-8-sig", errors="replace").strip(" \t\r\n\ufffd")
    return command.casefold() in {"exit", "/exit", "退出"}


@contextmanager
def terminal_input():
    """在 WSL 终端按字符编辑输入，并按实际显示宽度重绘。"""
    if not (sys.platform.startswith("linux") and sys.stdin.isatty()):
        yield
        return

    try:
        import readline  # 导入后，交互式 input() 使用 Readline 重绘输入行。
    except ImportError:
        pass
    else:
        # 兼容旧终端以 GB18030 发送的“退出”。在 UTF-8 模式下，
        # Readline 会丢掉这段按键序列末尾的无效字节，所以先映射整个词。
        readline.parse_and_bind(r'"\xCD\xCB\xB3\xF6": "退出"')
        yield
        return

    # 精简的 Python 安装可能缺少 Readline，保留原来的内核退格处理。
    import termios

    previous = termios.tcgetattr(sys.stdin)
    current = previous.copy()
    # Linux 的 IUTF8 标志在部分 Python 版本中未由 termios 导出。
    current[0] |= getattr(termios, "IUTF8", 0x4000)
    termios.tcsetattr(sys.stdin, termios.TCSANOW, current)
    try:
        yield
    finally:
        termios.tcsetattr(sys.stdin, termios.TCSANOW, previous)


def interactive_session(label, answer_question):
    api_key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
    if not api_key:
        print("缺少 DEEPSEEK_API_KEY，请在当前 Bash 终端设置并 export 后重试。", file=sys.stderr)
        return 1
    if not api_key.isascii() or any(char.isspace() or not char.isprintable() for char in api_key):
        print("API Key 中有异常字符，请重新复制并设置。", file=sys.stderr)
        return 1

    sys.stdin.reconfigure(errors="surrogateescape")
    print(f"{label}（{MODEL}）。每次独立提问，输入 exit、/exit、“退出”或按 Ctrl+C 结束。")
    try:
        with terminal_input():
            while True:
                question = input("你：").strip()
                if is_exit_command(question):
                    break
                if not question:
                    continue
                try:
                    print(answer_question(question, api_key=api_key), end="\n\n")
                except (RuntimeError, ValueError) as error:
                    print(error, file=sys.stderr)
    except (EOFError, KeyboardInterrupt):
        print()
    return 0


def main():
    parser = argparse.ArgumentParser(description="DeepSeek 问答与 netlog 命令预览")
    parser.add_argument("--input", metavar="日志路径", help="根据中文问题预览该输入的 netlog 查询命令")
    options = parser.parse_args()
    if options.input == "":
        parser.error("--input 需要非空路径")
    if options.input is not None:
        from netlog_query import query_session
        return query_session(options.input, preview=True)

    def teach(question, *, api_key):
        return f"DeepSeek：{ask_deepseek(question, api_key=api_key)}"

    return interactive_session("DeepSeek toy", teach)


if __name__ == "__main__":
    sys.exit(main())
