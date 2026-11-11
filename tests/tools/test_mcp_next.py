#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""Offline Next setup tests: local HTTP, synthetic FAT/firmware, no external media."""

import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import shutil
import sys
import threading
import time
import zipfile

sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[3])
from client import Client


def main():
    output = Path(sys.argv[2]).resolve()
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    with Client(sys.argv[1], output) as client:
        client.call("createSession", sessionId="setup", profile="48k")
        client.call("createSession", expect_error=True, sessionId="missing", profile="next")
        client.call("createMedia", output="fixture.img", filesystem="fat", overwrite=False)
        image = output / "fixture.img"
        client.call("insertSdCard", sessionId="setup", image=str(image), readOnly=False)
        client.call("makeSdDirectory", sessionId="setup", path="/machines/next")
        for name, size in (("enNextZX.rom", 65536), ("enNxtmmc.rom", 8192), ("enAltZX.rom", 32768)):
            client.call("writeSdFile", sessionId="setup", path="/machines/next/" + name, hexData="00" * size)
        client.call("createNextSession", expect_error=True, sessionId="conflict", image=str(image), readOnly=True)
        client.call("ejectSdCard", sessionId="setup")
        archive = output / "fixture.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as zipped:
            zipped.write(image, "subdirectory/cspect-next-1gb.img")
        payload = archive.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                requests.append((self.path, self.headers.get("Range")))
                if self.path == "/redirect":
                    self.send_response(302)
                    self.send_header("Location", "/sn-emulator-test.zip")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                if self.path == "/stall":
                    time.sleep(0.2)
                data = payload
                if self.path == "/page":
                    data = b'<html><a href="sn-emulator-test.zip">SD image</a></html>'
                if self.path == "/invalid":
                    data = b"not a zip"
                partial = self.path in ("/resume", "/ignore-range", "/bad-range")
                range_header = self.headers.get("Range")
                start = int(range_header.split("=")[1][:-1]) if range_header else 0
                ranged = start and self.path != "/ignore-range"
                self.send_response(206 if ranged else 200)
                if ranged:
                    first = start + (1 if self.path == "/bad-range" else 0)
                    self.send_header("Content-Range", f"bytes {first}-{len(data) - 1}/{len(data)}")
                self.send_header("ETag", '"fixture-v1"')
                self.send_header("Content-Length", str(len(data) - start if ranged else len(data)))
                self.end_headers()
                try:
                    if partial and not range_header:
                        self.wfile.write(data[:len(data) // 2])
                        self.wfile.flush()
                        self.close_connection = True
                    else:
                        self.wfile.write(data[start:] if ranged else data)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            discovered = client.call("discoverNextDistribution", pageUrl=base + "/page", maxMilliseconds=5000)
            assert discovered["url"] == base + "/sn-emulator-test.zip"
            direct = client.call("downloadFile", url=base + "/redirect", output="redirect.zip",
                                 expectedSha256=digest, maxMilliseconds=5000, overwrite=False)
            assert direct["sha256"] == digest and (output / "redirect.zip").read_bytes() == payload
            for route in ("resume", "ignore-range", "bad-range"):
                name = route + ".zip"
                client.call("downloadFile", expect_error=True, url=base + "/" + route, output=name,
                            expectedSha256=digest, maxMilliseconds=5000, overwrite=False)
                partial = output / (name + ".part")
                size = partial.stat().st_size
                assert 0 < size < len(payload) and not (output / name).exists()
                info = client.call("downloadFile", expect_error=(route == "bad-range"), url=base + "/" + route,
                                   output=name, expectedSha256=digest, maxMilliseconds=5000, overwrite=False)
                if route == "bad-range":
                    assert partial.stat().st_size == size
                else:
                    assert (output / name).read_bytes() == payload
                    if route == "resume":
                        assert info["resumedFrom"] == size
            client.call("downloadFile", expect_error=True, url=base + "/sn-emulator-test.zip", output="checksum.zip",
                        expectedSha256="00" * 32, maxMilliseconds=5000, overwrite=False)
            assert not (output / "checksum.zip").exists()
            client.call("downloadFile", expect_error=True, url=base + "/stall", output="timeout.zip",
                        expectedSha256="", maxMilliseconds=30, overwrite=False)
            client.call("downloadFile", expect_error=True, url="http://example.com/file", output="insecure.zip",
                        expectedSha256="", maxMilliseconds=100, overwrite=False)
            prepared = client.call("prepareNextSdCard", url=base + "/sn-emulator-test.zip", entryName="",
                                   output="", maxMilliseconds=5000, overwrite=False)
            prepared_image = Path(prepared["path"])
            assert prepared_image.read_bytes() == image.read_bytes()
            assert len(prepared["roms"]) == 3
            count = len(requests)
            assert client.call("prepareNextSdCard", url="", entryName="", output="",
                               maxMilliseconds=5000, overwrite=False)["cached"]
            assert len(requests) == count

            client.call("createSession", sessionId="next", profile="next")
            client.call("createNextSession", sessionId="shared", image=str(prepared_image), readOnly=True)
            client.call("createNextSession", expect_error=True, sessionId="writer-conflict", image=str(prepared_image), readOnly=False)
            client.call("writeSdFile", expect_error=True, sessionId="next", path="/bad.txt", hexData="00")
            assert len(client.call("readNextRegisters", sessionId="next")["values"]) == 256
            client.call("writeNextRegister", sessionId="next", index=0x4A, value=0xE3)
            assert client.call("readNextRegisters", sessionId="next")["values"][0x4A] == 0xE3
            assert any(entry["name"].lower() == "machines" for entry in client.call("listSdFiles", sessionId="next", path="/")["entries"])
            client.call("cloneSession", sessionId="next", newSessionId="next-clone")
            assert client.call("sessionStatus", sessionId="next-clone")["profile"] == "next"
            client.call("destroySession", sessionId="next-clone")

            writable = output / "writable.img"
            shutil.copyfile(prepared_image, writable)
            client.call("createNextSession", sessionId="writer", image=str(writable), readOnly=False)
            client.call("makeSdDirectory", sessionId="writer", path="/ai")
            client.call("writeSdFile", sessionId="writer", path="/ai/hello.txt", hexData=b"hello next".hex())
            assert client.call("readSdFile", sessionId="writer", path="/ai/hello.txt", offset=0, length=0, asText=True)["text"] == "hello next"
            client.call("renameSdFile", sessionId="writer", path="/ai/hello.txt", destination="/ai/renamed.txt")
            client.call("removeSdFile", sessionId="writer", path="/ai/renamed.txt")
            client.call("removeSdFile", sessionId="writer", path="/ai")
            client.call("cloneSession", expect_error=True, sessionId="writer", newSessionId="unsafe-clone")
            protected = output / "protected.img"
            shutil.copyfile(image, protected)
            original = hashlib.sha256(protected.read_bytes()).hexdigest()
            client.call("prepareNextSdCard", expect_error=True, url=base + "/invalid", entryName="", output=str(protected),
                        maxMilliseconds=5000, overwrite=True)
            assert hashlib.sha256(protected.read_bytes()).hexdigest() == original
            client.call("prepareNextSdCard", expect_error=True, url=base + "/sn-emulator-test.zip", entryName="", output=str(prepared_image),
                        maxMilliseconds=5000, overwrite=True)
            client.call("runFrames", sessionId="next", frameCount=1, maxMilliseconds=1000)
            (output / "network-validation.json").write_text(json.dumps(requests, indent=2))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
    print("Verified resumable downloads, redirects, integrity, deadlines, safe extraction, Next sessions and SD files")


if __name__ == "__main__":
    main()
