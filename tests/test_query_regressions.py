"""Run from WSL with python3 -m unittest discover -s tests -p 'test_query*.py'."""

import io
import json
import random
import shlex
import subprocess
import sys
import tempfile
import unittest
from collections import Counter
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import netlog_query as query

BINARY = ROOT / "build/netlog"


def plan(args, rank=None, top=None):
    return json.dumps(dict(args=args, rank_by=rank, top_in_peak=top))


class QueryRegressions(unittest.TestCase):
    def test_random_reference_and_thread_counts(self):
        for seed in range(12):
            with self.subTest(seed=seed), tempfile.TemporaryDirectory() as folder:
                rng = random.Random(seed)
                files = [[], [], [], []]
                matches = []
                for _ in range(320):
                    ts = rng.randrange(300)
                    dev = rng.choice(["A", "B"])
                    level = rng.choice(["INFO", "ERROR", "WARN"])
                    event = rng.choice(["X", "Y", "Z", "A"])
                    bucket = rng.choice(files)
                    bucket.append(f"{ts} {dev} {level} {event} 10")
                    if dev == "B" and 31 <= ts < 249:
                        matches.append((ts, level, event))
                diagnostics = []
                for i, lines in enumerate(files):
                    path = Path(folder) / f"{i}.log"
                    index = rng.randrange(len(lines))
                    lines.insert(index, "broken")
                    path.write_bytes("\r\n".join(lines).encode())
                    diagnostics.append(f"{path}:{index + 1}: expected exactly 5 fields")
                groups = Counter(ts // 30 * 30 for ts, _, _ in matches)
                errors = Counter(ts // 30 * 30 for ts, lv, _ in matches if lv == "ERROR")
                frequencies = Counter(event for _, _, event in matches)
                leaders = sorted(frequencies, key=lambda e: (-frequencies[e], e))[:2]
                args = ["--device", "B", "--since", "31", "--until", "249", "--bucket", "30", "--top", "2"]
                baseline = None
                for threads in (1, 2, 8):
                    run = subprocess.run([BINARY, "--input", folder, "--threads", str(threads), *args],
                                         text=True, capture_output=True, check=True)
                    self.assertEqual(run.stderr.splitlines(), diagnostics)
                    self.assertIn(f"read_lines=324\nvalid_records=320\ninvalid_records=4\nmatched_records={len(matches)}\n", run.stdout)
                    windows, top = run.stdout.split("start_ts requests errors error_rate avg_us max_us\n")[1].split("top_events (up to 2):\n")
                    expected = [(start, groups[start], errors[start]) for start in sorted(groups)]
                    actual = [tuple(map(int, line.split()[:3])) for line in windows.splitlines()]
                    self.assertEqual(actual, expected)
                    self.assertEqual(top.splitlines(), [f"{e} {frequencies[e]}" for e in leaders])
                    if baseline is None:
                        baseline = run.stdout
                    self.assertEqual(run.stdout, baseline)

    def test_one_row_spans_two_nominal_cuts(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "long.log"
            with path.open("wb") as stream:
                stream.write(b"1 d INFO LONG 7" + b" " * (32 * 1024 * 1024) + b"\r\nbad\n2 d ERROR LAST 3")
            for threads in (1, 8):
                run = subprocess.run([BINARY, "--input", path, "--threads", str(threads)],
                                     capture_output=True, text=True, check=True)
                self.assertIn("read_lines=3\nvalid_records=2\ninvalid_records=1\n", run.stdout)
                self.assertIn("d 2 1 50.00% 5.00 7\n", run.stdout)
                self.assertEqual(run.stderr, f"{path}:2: expected exactly 5 fields\n")

    def test_peak_count_and_rate_choose_different_events(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "peaks.log"
            path.write_text("0 dev ERROR A 1\n"*3 + "0 dev INFO B 1\n"*7 + "60 dev ERROR C 1\n")
            for rank, interval, event in [("errors", "[0, 60)", "B 7"), ("error_rate", "[60, 120)", "C 1")]:
                with self.subTest(rank=rank), redirect_stdout(io.StringIO()):
                    result = query.execute_plan(plan(["--bucket", "60"], rank, 1), path, BINARY)
                    self.assertIn(interval, result)
                    self.assertTrue(result.endswith(event))

    def test_illegal_model_replies_do_not_start_process(self):
        malformed = ["not json", "[]", '{"args":[]}', plan(["--top","2","--top","3"]),
                     plan(["--until",str(2**63)]), plan(["--bucket","0"]), plan(["--since","2","--until","1"]),
                     plan(["--threads","8"]), plan(["--bucket","60"],"errors",True), plan([], "errors")]
        with patch.object(query, "run_netlog") as run:
            for reply in malformed:
                with self.subTest(reply=reply), self.assertRaises(ValueError):
                    query.execute_plan(reply, "unused", BINARY)
            run.assert_not_called()

    def test_preview_quotes_path_and_does_not_scan(self):
        path = "local data/O'Brien.log"
        with patch.object(query, "run_netlog") as run:
            output = query.render_plan(plan(["--top","2"]), path, str(BINARY))
            self.assertEqual(shlex.split(output), [str(BINARY), "--input",path,"--top","2"])
            run.assert_not_called()

    def test_native_overflow_stops_before_second_scan(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "overflow.log"
            path.write_text("1 d INFO OK 18446744073709551615\n2 d INFO OK 1\n")
            with patch.object(query, "run_netlog", wraps=query.run_netlog) as run, redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(RuntimeError, "状态码 4"):
                    query.execute_plan(plan(["--bucket","60"],"errors",2), path, BINARY)
                self.assertEqual(run.call_count, 1)


if __name__ == "__main__":
    unittest.main()
