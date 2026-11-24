"""在 WSL 项目根目录用 python3 tests/test_query_execution.py 运行，无需 API Key。"""

import io
import json
import sys
import tempfile
import unittest
from contextlib import chdir, redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import netlog_query as query


class QueryExecutionTests(unittest.TestCase):
    def execute(self, path, args, rank=None, top=None):
        reply = json.dumps(dict(args=args, rank_by=rank, top_in_peak=top))
        with redirect_stdout(io.StringIO()), patch.object(query, "run_netlog", wraps=query.run_netlog) as run:
            result = query.execute_plan(reply, path, ROOT / "build/netlog")
        return result, [call.args[0] for call in run.call_args_list]

    def test_session_default_and_relative_override_from_elsewhere(self):
        plan = json.dumps(dict(args=["--top", "1"], rank_by=None, top_in_peak=None))
        def ask_once(label, answer):
            return answer("查询高频事件", api_key="offline")

        with tempfile.TemporaryDirectory() as folder, chdir(folder), redirect_stdout(io.StringIO()):
            Path("my log").write_text("0 d INFO CUSTOM 1\n", encoding="utf-8")
            with patch("deepseek_toy.ask_deepseek", return_value=plan) as ask, \
                    patch("deepseek_toy.interactive_session", side_effect=ask_once):
                self.assertIn("valid_records=5", query.query_session())
                self.assertIn("CUSTOM 1", query.query_session("my log"))
                with redirect_stderr(io.StringIO()):
                    self.assertEqual(query.query_session("missing file"), 2)
                self.assertEqual(ask.call_count, 2)

    def test_file_with_spaces_and_shell_characters(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "自选 样例;test.log"
            path.write_text("0 dev INFO A 1\n1 dev ERROR B 2\n", encoding="utf-8")
            result, scans = self.execute(path, ["--top", "1"])
            self.assertIn("valid_records=2", result)
            self.assertTrue(result.endswith("A 1"))
            self.assertEqual(len(scans), 1)
            self.assertEqual(scans[0][2], str(path))

    def test_directory_peak_and_scope(self):
        args = ["--device", "dev01", "--since", "120", "--until", "300", "--bucket", "60"]
        result, scans = self.execute(ROOT / "data/incident", args, "error_rate", 2)
        self.assertIn("[180, 240)", result)
        self.assertIn("4 次请求，3 次错误，错误率 75.00%", result)
        self.assertTrue(result.endswith("TIMEOUT 2\nCRC_ERROR 1"))
        self.assertEqual(len(scans), 2)
        self.assertEqual(scans[1][3:], ["--device", "dev01", "--since", "180", "--until", "240", "--top", "2"])

    def test_clip_both_ends_and_keep_global_ranking(self):
        args = ["--device", "dev01", "--since", "185", "--until", "225", "--bucket", "60", "--top", "4"]
        result, scans = self.execute(ROOT / "data/incident", args, "errors", 2)
        self.assertIn("整个查询时段的事件排行\ntop_events (up to 4)", result)
        self.assertIn("峰值窗口的事件排行\ntop_events (up to 2)", result)
        self.assertEqual(scans[1][3:], ["--device", "dev01", "--since", "185", "--until", "225", "--top", "2"])

    def test_no_match_skips_second_scan(self):
        result, scans = self.execute(ROOT / "data/sample.log", ["--device", "missing", "--bucket", "60"], "errors", 2)
        self.assertIn("没有匹配记录", result)
        self.assertEqual(len(scans), 1)

    def test_max_timestamp_can_participate_in_second_scan(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "max.log"
            path.write_text(f"{query.MAX_SECOND} dev ERROR LAST 1\n", encoding="utf-8")
            result, scans = self.execute(path, ["--bucket", "60"], "errors", 1)
            self.assertNotIn("--until", scans[1])
            self.assertTrue(result.endswith("LAST 1"))

    def test_metrics_ties_and_display_rounding(self):
        report = ("time_windows (60 seconds each):\nstart_ts requests errors error_rate avg_us max_us\n"
                  "0 30001 3 0.01% 1.00 1\n60 10000 1 0.01% 1.00 1\n"
                  "120 10000 1 0.01% 1.00 1\ntop_events (up to 3):\n")
        self.assertEqual(query.peak_window(report, 60, "errors")[0], 0)
        self.assertEqual(query.peak_window(report, 60, "error_rate")[0], 60)

    def test_non_queries_never_run_netlog(self):
        with patch.object(query, "run_netlog") as run:
            for key in ["clarify", "unsupported"]:
                message = json.dumps({key: "说明"})
                self.assertIn("说明", query.execute_plan(message, "unused", "unused"))
            invalid = json.dumps(dict(args=["--input", "/other"], rank_by=None, top_in_peak=None))
            with self.assertRaises(ValueError):
                query.execute_plan(invalid, "unused", "unused")
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
