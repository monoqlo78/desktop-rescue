"""Safe real-SDK stdio round trip: discovery and read-only status only."""
import os
from pathlib import Path
import sys
import unittest

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


class ProtocolTests(unittest.IsolatedAsyncioTestCase):
    async def test_discovery_and_status(self):
        root = Path(__file__).resolve().parents[1]
        executable = root / "artifacts" / "win-x64" / "desktop-rescue.exe"
        self.assertTrue(executable.is_file(), "Build the native executable first")
        parameters = StdioServerParameters(
            command=sys.executable,
            args=[str(root / "mcp" / "server.py")],
            env={"DESKTOP_RESCUE_EXE": str(executable)},
        )
        async with stdio_client(parameters) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                await session.initialize()
                tools = await session.list_tools()
                self.assertEqual({tool.name for tool in tools.tools}, {"start", "status", "recover", "stop"})
                response = await session.call_tool("status", {})
                self.assertTrue(response.content)
                self.assertIsInstance(response.isError, bool)
                # A stopped resident is an explicit tool error, not a success-shaped default.
                if response.isError:
                    self.assertIn("not_running", response.content[0].text)


if __name__ == "__main__":
    unittest.main()
