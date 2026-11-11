#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""Drive the real MCP process; check session isolation, traps, media and artifacts."""

import base64
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import wave

sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[3])
from client import Client


def main():
    executable = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)

    with Client(executable, output) as client:
        tools = client.request("tools/list")["result"]["tools"]
        assert len(tools) >= 70, len(tools)
        assert len({tool["name"] for tool in tools}) == len(tools)
        assert all(tool.get("description") and tool["inputSchema"]["additionalProperties"] is False for tool in tools)
        catalog = {tool["name"]: tool for tool in tools}
        profiles = client.call("listMachineProfiles")["profiles"]
        assert all(next(p for p in profiles if p["id"] == name)["embeddedRoms"] for name in ("48k", "128k"))

        client.call("createSession", sessionId="first", profile="48k")
        client.call("createSession", sessionId="second", profile="128k")
        client.call("createSession", expect_error=True, sessionId="first", profile="48k")
        client.call("createSession", expect_error=True, sessionId="bad", profile="not-a-machine")
        assert len(client.call("listSessions")["sessions"]) == 2
        client.call("pokeMemory", sessionId="first", address=32768, hexData="3e 42 32 00 90 c3 00 80", allowRomWrite=False)
        assert client.call("peekMemory", sessionId="second", address=32768, length=8)["hex"] != "3e42320090c30080"
        client.call("useSession", sessionId="first")
        assert client.call("peekMemory", sessionId="", address=32768, length=2)["hex"] == "3e42"
        original_rom = client.call("peekMemory", sessionId="first", address=0, length=4)["hex"]
        client.call("pokeMemory", expect_error=True, sessionId="first", address=0, hexData="00000000", allowRomWrite=False)
        assert client.call("peekMemory", sessionId="first", address=0, length=4)["hex"] == original_rom
        client.call("pokeMemory", expect_error=True, sessionId="first", address=65535, hexData="1234", allowRomWrite=False)
        client.call("peekMemory", expect_error=True, sessionId="first", address=-1, length=2)
        client.call("pokeMemory", expect_error=True, sessionId="first", address=32768, hexData="f", allowRomWrite=False)
        missing = client.request("tools/call", {"name": "peekMemory", "arguments": {"sessionId": "first"}})
        assert missing["result"]["isError"]
        assert client.request("tools/call", {"name": "absent", "arguments": {}})["error"]["code"] == -32602

        client.call("setRegister", sessionId="first", name="PC", value=32768)
        client.call("setRegister", sessionId="first", name="SP", value=65500)
        client.call("setRegister", sessionId="first", name="IFF1", value=0)
        client.call("setRegister", sessionId="first", name="IFF2", value=0)
        client.call("setRegister", expect_error=True, sessionId="first", name="A", value=256)
        client.call("configureTrace", sessionId="first", enabled=True, capacity=16)
        watch = client.call("addBreakpoint", sessionId="first", type="write", address=36864, mask=65535,
                            bankId=-1, page1k=-1, temporary=False, condition="A==0x42")["breakpointId"]
        stopped = client.call("runFrames", sessionId="first", frameCount=10, maxMilliseconds=1000)
        assert stopped["lastPause"]["reason"] == "memory", stopped
        assert client.call("peekMemory", sessionId="first", address=36864, length=1)["hex"] == "42"
        assert client.call("listBreakpoints", sessionId="first")["breakpoints"][0]["hitCount"] == 1
        assert client.call("readTrace", sessionId="first", count=16)["entries"]
        client.call("enableBreakpoint", sessionId="first", breakpointId=watch, enabled=False)
        client.call("removeBreakpoint", sessionId="first", breakpointId=watch)
        client.call("setSymbol", sessionId="first", address=32768, name="loop")
        assert client.call("disassembleMemory", sessionId="first", address=32768, count=2)["instructions"][0]["symbol"] == "loop"
        assert client.call("searchMemory", sessionId="first", address=32768, length=8,
                           hexPattern="3e40", hexMask="fff0", maxResults=4)["addresses"] == [32768]
        assert client.call("compareMemory", sessionId="first", otherSessionId="second", address=32768,
                           length=8, maxResults=4)["differences"]
        client.call("cloneSession", sessionId="first", newSessionId="clone")
        assert not client.call("compareMemory", sessionId="first", otherSessionId="clone", address=16384,
                               length=49152, maxResults=4)["differences"]
        client.call("pokeMemory", sessionId="clone", address=32768, hexData="00", allowRomWrite=False)
        assert client.call("peekMemory", sessionId="first", address=32768, length=1)["hex"] == "3e"

        banks = client.call("listMemoryBanks", sessionId="second")["banks"]
        ram = next(bank["id"] for bank in banks if bank["writable"] and bank["size"] >= 131072)
        client.call("writeMemoryBank", sessionId="second", bankId=ram, offset=7 * 16384, hexData="badc0ffe", allowRomWrite=False)
        client.call("writePort", sessionId="second", port=32765, value=7)
        assert client.call("peekMemory", sessionId="second", address=49152, length=4)["hex"] == "badc0ffe"
        assert client.call("readMemoryBank", sessionId="second", bankId=ram, offset=7 * 16384, length=4)["hex"] == "badc0ffe"
        assert len(client.call("memoryMap", sessionId="second")["slots"]) == 64

        assembled = client.call("assemble", source="LD A,42\nLD (36864),A\nRET", origin=33000, z80n=False)["hex"]
        decoded = client.call("disassembleData", hexData=assembled, origin=33000, z80n=False)["instructions"]
        assert len(decoded) == 3 and not decoded[-1]["truncated"]
        assert client.call("disassembleData", hexData="cd", origin=0, z80n=False)["instructions"][0]["truncated"]
        before = client.call("peekMemory", sessionId="first", address=34000, length=8)["hex"]
        client.call("assembleToMemory", expect_error=True, sessionId="first", source="NOP\nNOT_AN_INSTRUCTION", address=34000)
        assert client.call("peekMemory", sessionId="first", address=34000, length=8)["hex"] == before
        basic = client.call("compileBasic", source='10 PRINT "MCP"\n20 RANDOMIZE USR 32768\n', dialect="48")["hex"]
        listing = client.call("decompileBasic", hexData=basic, dialect="48")["source"]
        assert "MCP" in listing
        assert client.call("compileBasic", source=listing, dialect="48")["hex"] == basic
        hidden = client.call("compileBasic", source=r"10 CLEAR 99999\x0E\x00\x00\xC5\x62\x00" + "\n!raw D6\n", dialect="48")["hex"]
        inspected = client.call("decompileBasic", hexData=hidden, dialect="48")
        assert inspected["listing"] == "10 CLEAR 99999\n", inspected
        assert inspected["effectiveListing"] == "10 CLEAR 25285\n", inspected
        assert inspected["numericMismatchCount"] == 1 and inspected["trailingHex"] == "d6"
        assert inspected["numericConstants"][0]["storedValue"] == 25285
        assert client.call("compileBasic", source=inspected["source"], dialect="48")["hex"] == hidden
        literals = client.call("compileBasic", source='10 PRINT "123": REM 456\n20 LET x=1.5: PRINT BIN 1010\n', dialect="48")["hex"]
        inspected = client.call("decompileBasic", hexData=literals, dialect="48")
        assert len(inspected["numericConstants"]) == 2 and inspected["numericMismatchCount"] == 0, inspected
        client.call("compileBasicToMemory", sessionId="first", source='10 PRINT "MCP"', address=34000, dialect="48")
        array = client.call("compileBasicArray", source="2\n1 2\n", characterArray=False)["hex"]
        assert "2" in client.call("decompileBasicArray", hexData=array, characterArray=False)["source"]

        # A tiny generated tape checks headers, checksum, BASIC listings and archive editing.
        payload = bytes.fromhex(basic)
        header = bytes([0]) + b"MCP TEST  " + struct.pack("<HHH", len(payload), 10, len(payload))
        tape = bytearray()
        for flag, body in ((0, header), (255, payload)):
            block = bytes([flag]) + body
            checksum = 0
            for byte in block:
                checksum ^= byte
            block += bytes([checksum])
            tape += struct.pack("<H", len(block)) + block
        tape_path = output / "fixture.tap"
        tape_path.write_bytes(tape)
        client.call("loadMedia", sessionId="first", path=str(tape_path))
        blocks = client.call("inspectTape", sessionId="first")["blocks"]
        assert len(blocks) == 2 and all(block["checksumValid"] for block in blocks)
        assert client.call("readTapeBlock", sessionId="first", blockIndex=1, payloadOnly=True)["hex"] == basic
        entries = client.call("inspectMedia", path=str(tape_path), filesystem="")["entries"]
        basic_entry = next(entry["name"] for entry in entries if entry["name"].startswith("basic/"))
        assert "MCP" in client.call("readMediaEntry", path=str(tape_path), entry=basic_entry, filesystem="",
                                    offset=0, length=0, asText=True)["text"]
        client.call("extractMediaEntry", path=str(tape_path), entry=basic_entry, filesystem="", output="listing.bas", overwrite=False)
        client.call("saveMedia", sessionId="first", path="saved.szx", overwrite=False)
        client.call("saveMedia", expect_error=True, sessionId="first", path="saved.szx", overwrite=False)
        client.call("loadMedia", sessionId="clone", path=str(output / "saved.szx"))
        client.call("createMedia", output="blank.tap", filesystem="", overwrite=False)
        client.call("putMediaEntry", path=str(output / "blank.tap"), entry="files/BYTES", filesystem="", hexData="1234",
                    attributesJson='{"type":3,"param1":32768}', output="edited.tap", overwrite=False)
        assert client.call("readMediaEntry", path=str(output / "edited.tap"), entry="files/BYTES", filesystem="",
                           offset=0, length=0, asText=False)["hex"] == "1234"
        client.call("removeMediaEntry", path=str(output / "edited.tap"), entry="files/BYTES", filesystem="",
                    output="removed.tap", overwrite=False)

        client.call("exportMemory", sessionId="first", address=32768, length=8, path="memory.bin", overwrite=False)
        client.call("importMemory", sessionId="clone", address=35000, path=str(output / "memory.bin"))
        assert client.call("peekMemory", sessionId="clone", address=35000, length=8)["hex"] == "3e42320090c30080"
        png_reply = client.request("tools/call", {"name": "captureScreen", "arguments": {
            "sessionId": "first", "path": "screen.png", "overwrite": False,
        }})["result"]
        assert not png_reply["isError"]
        image = next(block for block in png_reply["content"] if block["type"] == "image")
        assert base64.b64decode(image["data"]) == (output / "screen.png").read_bytes()
        assert (output / "screen.png").read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
        client.call("renderGraphics", sessionId="first", address=32768, width=8, height=8, count=1,
                    stride=0, columns=1, path="tile.png", overwrite=False)
        client.call("captureAudio", sessionId="clone", frameCount=2, maxMilliseconds=1000, path="audio.wav", overwrite=False)
        with wave.open(str(output / "audio.wav")) as audio:
            assert audio.getnchannels() == 2 and audio.getsampwidth() == 2 and audio.getnframes() > 0
        client.call("clearBreakpoints", sessionId="first")
        client.call("destroySession", sessionId="clone")
        client.call("sessionStatus", expect_error=True, sessionId="clone")

        # Notifications must never execute a tool, and stdout must contain only replies.
        client.notify("tools/call", {"name": "destroySession", "arguments": {"sessionId": "first"}})
        assert client.request("ping")["result"] == {}
        assert client.request("tools/list", {"_meta": {"progressToken": "trace-1"}})["result"]["tools"]
        assert client.call("sessionStatus", sessionId="first")["sessionId"] == "first"
        client.call("pokeMemory", sessionId="first", address=40000, hexData="18fe", allowRomWrite=False)
        client.call("setRegister", sessionId="first", name="PC", value=40000)
        bounded = client.call("runToAddress", sessionId="first", address=40010, maxFrames=10000, maxMilliseconds=1)
        assert bounded["stopReason"] == "timeBudget" and bounded["paused"]

        client.call("createSession", sessionId="basic", profile="48k")
        client.call("runFrames", sessionId="basic", frameCount=100, maxMilliseconds=5000)
        client.call("setBasicProgram", sessionId="basic", source="10 POKE 60000,42\n")
        assert "POKE" in client.call("getBasicProgram", sessionId="basic")["source"]
        client.call("tapKeys", sessionId="basic", keys="R", holdFrames=3, releaseFrames=3)
        client.call("tapKeys", sessionId="basic", keys="ENTER", holdFrames=3, releaseFrames=3)
        client.call("runFrames", sessionId="basic", frameCount=20, maxMilliseconds=1000)
        assert client.call("peekMemory", sessionId="basic", address=60000, length=1)["hex"] == "2a"

        client.call("createSession", sessionId="io", profile="48k")
        client.call("assembleToMemory", sessionId="io", address=32768,
                    source="LD A,16\nOUT (254),A\nXOR A\nOUT (254),A\nJP 32768")
        client.call("setRegister", sessionId="io", name="PC", value=32768)
        trace = client.call("captureIo", sessionId="io", frameCount=2, maxMilliseconds=1000,
                            mask=255, value=254, includeReads=False, includeWrites=True, maxEvents=5)["io"]
        assert len(trace["events"]) == 5 and trace["truncated"] and trace["totalEvents"] > 5
        assert all(event["port"] & 255 == 254 and event["write"] for event in trace["events"])
        audio = client.call("captureAudio", sessionId="io", frameCount=2, maxMilliseconds=1000,
                            path="io.wav", overwrite=False)
        assert audio["ioWrites"]["totalEvents"] > 0
        client.call("runFrames", sessionId="io", frameCount=1, maxMilliseconds=1000)

        (output / "catalog.json").write_text(json.dumps(catalog, indent=2))

    with Client(executable, output, native=True) as client:
        discovery = client.request("server/discover")["result"]
        assert discovery["supportedVersions"] == ["2026-07-28"]
        assert len(client.request("tools/list")["result"]["tools"]) == len(tools)
        client.call("createSession", sessionId="native", profile="48k")
        assert client.call("sessionStatus", sessionId="native")["profile"] == "48k"

    malformed = subprocess.run([str(executable)], input='[]\n{\n', text=True, capture_output=True, check=True)
    assert [json.loads(line)["error"]["code"] for line in malformed.stdout.splitlines()] == [-32600, -32700]
    print(f"Verified {len(tools)} MCP tools: sessions, physical paging, traps, codecs, media and captures")


if __name__ == "__main__":
    main()
