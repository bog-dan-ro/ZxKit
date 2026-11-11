#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""Small dependency-free stdio client for regression tests and reproducible studies."""

import json
from pathlib import Path
import selectors
import subprocess


class Client:
    def __init__(self, executable, output_directory, native=False, timeout=40):
        self.process = subprocess.Popen(
            [str(Path(executable).resolve()), "--output-dir", str(Path(output_directory).resolve())],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8",
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.counter = 0
        self.native = native
        self.timeout = timeout
        self.exchanges = []
        if not native:
            reply = self.request("initialize", {
                "protocolVersion": "2025-11-25", "capabilities": {},
                "clientInfo": {"name": "zx-mcp-python", "version": "1.0"},
            })
            assert reply["result"]["protocolVersion"] == "2025-11-25", reply
            self.notify("notifications/initialized", {})

    def notify(self, method, params):
        self.process.stdin.write(json.dumps({"jsonrpc": "2.0", "method": method, "params": params}) + "\n")
        self.process.stdin.flush()

    def request(self, method, params=None):
        self.counter += 1
        params = dict(params or {})
        if self.native:
            params["_meta"] = {
                "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                "io.modelcontextprotocol/clientCapabilities": {},
            }
        request = {"jsonrpc": "2.0", "id": self.counter, "method": method, "params": params}
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        if not self.selector.select(timeout=self.timeout):
            raise TimeoutError(f"No response to {method}")
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError(f"Server exited with {self.process.poll()}")
        reply = json.loads(line)
        assert reply.get("id") == self.counter, reply
        self.exchanges.append({"request": request, "response": reply})
        return reply

    def call(self, tool_name, *, expect_error=False, **arguments):
        reply = self.request("tools/call", {"name": tool_name, "arguments": arguments})
        assert "result" in reply, reply
        result = reply["result"]
        assert result.get("isError", False) == expect_error, (tool_name, arguments, result)
        return result.get("structuredContent", {"error": result["content"][0]["text"]})

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise
        self.selector.close()
        self.process.stdout.close()
        assert self.process.returncode == 0, self.process.returncode

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.close()
