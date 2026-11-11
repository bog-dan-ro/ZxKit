# Portable stdio access

Use native MCP tools when your agent already has zx-mcp connected. Otherwise,
launch `zx-mcp` as a persistent child process with `--output-dir DIR`. The optional
`--roms DIR` supplies additional ROMs. No agent-specific configuration format is
required by the server.

The bundled `scripts/client.py` uses Python's standard library. It initializes
MCP, validates response IDs, returns tool `structuredContent`, detects tool
errors and preserves exchanges in `client.exchanges`. Its context manager closes
the server. It accepts an executable path and output directory, not a shell
command string. Sessions exist only while that process runs.

To inspect the current schemas:

```sh
zx-mcp --list-tools
```

This example works from a source or installed copy of the skill. Set
`ZX_MCP_SKILL` to the directory containing `SKILL.md`; set `ZX_MCP_SERVER` to
the executable path. Output defaults to `build/zx-mcp` relative to the working
directory.

```python
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
skill = Path(os.environ["ZX_MCP_SKILL"]).resolve()
server = Path(os.environ["ZX_MCP_SERVER"]).resolve()
output = Path("build/zx-mcp").resolve()
output.mkdir(parents=True, exist_ok=True)
sys.path.insert(0, str(skill / "scripts"))
from client import Client

with Client(server, output) as client:
    schemas = client.request("tools/list")["result"]["tools"]
    print([tool["name"] for tool in schemas])
    client.call("createSession", sessionId="reference", profile="48k")
    client.call("cloneSession", sessionId="reference", newSessionId="work")
    original = client.call("peekMemory", sessionId="reference", address=32768, length=4)
    client.call("pokeMemory", sessionId="work", address=32768,
                hexData="01020304", allowRomWrite=False)
    assert client.call("peekMemory", sessionId="reference", address=32768, length=4) == original
    assert client.call("peekMemory", sessionId="work", address=32768, length=4)["hex"] == "01020304"
    client.call("saveMedia", sessionId="work", path="work.szx", overwrite=True)
    for session_id in ("work", "reference"):
        client.call("destroySession", sessionId=session_id)
    (output / "session-example.json").write_text(json.dumps(client.exchanges, indent=2))
```

Normal execution tools accept a wall-clock budget up to 30000 ms; use shorter
budgets for interactive work. The client response timeout defaults to 40 seconds.
Download/preparation tools can run longer: pass an appropriately larger `timeout`
to `Client` when using them. The server's execution is synchronous; MCP
cancellation cannot interrupt an in-progress tool call.

PNG tool responses also contain image content for clients that render it. The
Python helper returns structured metadata; use the saved PNG file for viewing.
In `captureAudio`, PCM recording continues for the executed frames even when
the accompanying bounded I/O trace is truncated.
