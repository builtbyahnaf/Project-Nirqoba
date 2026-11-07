import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch

# Add benchmark directory to path
BENCH_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(BENCH_DIR))

import run_benchmark


class TestBenchmarkRunner(unittest.TestCase):
    def test_command_succeeds(self):
        code, output, timed_out = run_benchmark.run_command(
            [sys.executable, "-c", "print('hello world')"], timeout=10
        )
        self.assertEqual(code, 0)
        self.assertIn("hello world", output)
        self.assertFalse(timed_out)

    def test_command_exits_nonzero(self):
        code, output, timed_out = run_benchmark.run_command(
            [sys.executable, "-c", "import sys; sys.stderr.write('err'); sys.exit(42)"],
            timeout=10,
        )
        self.assertEqual(code, 42)
        self.assertIn("err", output)
        self.assertFalse(timed_out)

    def test_command_times_out_before_output(self):
        code, output, timed_out = run_benchmark.run_command(
            [sys.executable, "-c", "import time; time.sleep(5)"], timeout=1
        )
        self.assertTrue(timed_out)
        self.assertEqual(code, -1)

    def test_command_times_out_after_partial_stdout(self):
        code, output, timed_out = run_benchmark.run_command(
            [
                sys.executable,
                "-u",
                "-c",
                "import time, sys; sys.stdout.write('partial stdout'); sys.stdout.flush(); time.sleep(5)",
            ],
            timeout=1,
        )
        self.assertTrue(timed_out)
        self.assertIn("partial stdout", output)

    def test_command_times_out_after_partial_stderr(self):
        code, output, timed_out = run_benchmark.run_command(
            [
                sys.executable,
                "-u",
                "-c",
                "import time, sys; sys.stderr.write('partial stderr'); sys.stderr.flush(); time.sleep(5)",
            ],
            timeout=1,
        )
        self.assertTrue(timed_out)
        self.assertIn("partial stderr", output)

    def test_atomic_save_and_checkpoint_survives(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            out_path = Path(tmpdir) / "results.json"
            data = {"status": "ok", "value": 123}
            run_benchmark.atomic_save_json(out_path, data)
            self.assertTrue(out_path.exists())
            with open(out_path, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            self.assertEqual(loaded, data)

    def test_previous_successful_json_not_overwritten_on_failure(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            orig_path = Path(tmpdir) / "benchmark_results.json"
            orig_data = {"valid": True, "note": "golden"}
            orig_path.write_text(json.dumps(orig_data), encoding="utf-8")

            with patch("run_benchmark.RESULT_JSON", orig_path):
                # Run main pointing to orig_path with a test that fails validation
                test_args = [
                    "run_benchmark.py",
                    "--test",
                    "01",
                    "--impl",
                    "nirqoba",
                    "--repeats",
                    "1",
                    "--skip-compile",
                    "--output",
                    str(orig_path),
                ]
                with patch.object(sys, "argv", test_args):
                    with patch(
                        "run_benchmark.run_command",
                        return_value=(-1, "dummy output", True),
                    ):
                        ret = run_benchmark.main()
                        self.assertEqual(ret, 1)

            # Golden file must still be intact
            with open(orig_path, "r", encoding="utf-8") as f:
                current_data = json.load(f)
            self.assertEqual(current_data, orig_data)

    def test_nirqoba_7000ms_condition_label_fail(self):
        # Sample output for nirqoba test 01 where ALLOC_MS > 7000ms
        sample_output = """
TEST managed_object_allocation
IMPL nirqoba
OBJECTS 1000000
WARMUP_ROUNDS 1
MEASURE_ROUNDS 1
ROUND 0
ALLOC_MS 7050
TRAVERSE_MS 20
COLLECT_MS 50
CHECKSUM 499999500000
LIVE_AFTER 0
"""
        parsed = run_benchmark.parse_output(sample_output)
        problems = run_benchmark.validate_run("01", "nirqoba", parsed)
        self.assertTrue(any("FAIL" in p and "7000ms" in p for p in problems))

        # Sample output where ALLOC_MS <= 7000ms
        normal_output = """
TEST managed_object_allocation
IMPL nirqoba
OBJECTS 1000000
WARMUP_ROUNDS 1
MEASURE_ROUNDS 1
ROUND 0
ALLOC_MS 450
TRAVERSE_MS 20
COLLECT_MS 50
CHECKSUM 499999500000
LIVE_AFTER 0
"""
        parsed_ok = run_benchmark.parse_output(normal_output)
        problems_ok = run_benchmark.validate_run("01", "nirqoba", parsed_ok)
        self.assertEqual(problems_ok, [])


if __name__ == "__main__":
    unittest.main()
