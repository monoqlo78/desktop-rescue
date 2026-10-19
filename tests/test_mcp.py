import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "desktop_rescue_adapter", Path(__file__).resolve().parents[1] / "mcp" / "server.py"
)
adapter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(adapter)


class AdapterTests(unittest.TestCase):
    def execute(self, output, code=0):
        with (
            patch.dict(os.environ, {"DESKTOP_RESCUE_EXE": str(Path(__file__).resolve())}),
            patch.object(subprocess, "run") as run,
        ):
            run.return_value = subprocess.CompletedProcess([], code, json.dumps(output), "")
            result = adapter.execute("status")
            self.assertEqual(run.call_args.args[0][1], "status")
            self.assertFalse(run.call_args.kwargs.get("shell", False))
            self.assertEqual(run.call_args.kwargs["timeout"], 12)
            return result

    def test_status(self):
        self.assertEqual(self.execute({"ok": True, "code": "running"})["code"], "running")

    def test_failures_are_not_success(self):
        with self.assertRaises(RuntimeError):
            self.execute({"ok": False, "code": "not_running"}, 1)
        with self.assertRaises(RuntimeError):
            self.execute({"ok": True}, 1)
        with self.assertRaises(RuntimeError):
            self.execute({"wrong_shape": True})
        with self.assertRaises(RuntimeError):
            self.execute(["unexpected"])

    def test_no_arbitrary_commands(self):
        with patch.object(subprocess, "run") as run:
            with self.assertRaises(ValueError):
                adapter.execute("autostart enable")
            run.assert_not_called()

    def test_absolute_path_required(self):
        with patch.dict(os.environ, {"DESKTOP_RESCUE_EXE": "relative.exe"}):
            with self.assertRaises(ValueError):
                adapter.execute("status")

    def test_invalid_json(self):
        with (
            patch.dict(os.environ, {"DESKTOP_RESCUE_EXE": str(Path(__file__).resolve())}),
            patch.object(subprocess, "run", return_value=subprocess.CompletedProcess([], 0, "invalid", "")),
        ):
            with self.assertRaises(RuntimeError):
                adapter.execute("status")

    def test_timeout_propagates(self):
        with (
            patch.dict(os.environ, {"DESKTOP_RESCUE_EXE": str(Path(__file__).resolve())}),
            patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired([], 12)),
        ):
            with self.assertRaises(subprocess.TimeoutExpired):
                adapter.execute("status")


if __name__ == "__main__":
    unittest.main()
