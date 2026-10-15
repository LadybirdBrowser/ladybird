#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import runpy
import socket
import struct
import subprocess
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))
TIMEOUT_SECONDS = helpers["EVENT_TIMEOUT_SECONDS"]

# What each User-Agent preset's TLS ClientHello has to show. The cipher suites are the browser's, in its order,
# with any GREASE value left out. The extensions named are ones libcurl's own handshake never sends, and GREASE
# values are what Safari sends and Firefox doesn't.
# fmt: off
EXPECTATIONS = {
    "Safari macOS Desktop": {
        "cipher_suites": [
            0x1302, 0x1303, 0x1301,
            0xC02C, 0xC02B, 0xCCA9, 0xC030, 0xC02F, 0xCCA8,
            0xC00A, 0xC009, 0xC014, 0xC013,
            0x009D, 0x009C, 0x0035, 0x002F,
            0xC008, 0xC012, 0x000A,
        ],
        "extensions": {27},  # compress_certificate
        "grease": True,
    },
    "Firefox macOS Desktop": {
        "cipher_suites": [
            0x1301, 0x1303, 0x1302,
            0xC02B, 0xC02F, 0xCCA9, 0xCCA8, 0xC02C, 0xC030,
            0xC00A, 0xC009, 0xC013, 0xC014,
            0x009C, 0x009D, 0x002F, 0x0035,
        ],
        "extensions": {34},  # delegated_credentials
        "grease": False,
    },
}
# fmt: on


def is_grease(value):
    return (value & 0x0F0F) == 0x0A0A and (value >> 8) == (value & 0xFF)


def parse_client_hello(record):
    # One TLS record (type 22) holding one ClientHello handshake message (type 1).
    assert record[0] == 22, record[:5]
    body = record[5:]
    assert body[0] == 1, body[:4]
    offset = 4 + 2 + 32  # handshake header, client_version, random
    session_id_length = body[offset]
    offset += 1 + session_id_length
    (cipher_suites_length,) = struct.unpack_from("!H", body, offset)
    offset += 2
    cipher_suites = [struct.unpack_from("!H", body, offset + i)[0] for i in range(0, cipher_suites_length, 2)]
    offset += cipher_suites_length
    compression_length = body[offset]
    offset += 1 + compression_length
    (extensions_length,) = struct.unpack_from("!H", body, offset)
    offset += 2
    extensions = []
    end = offset + extensions_length
    while offset + 4 <= end:
        extension_type, extension_length = struct.unpack_from("!HH", body, offset)
        extensions.append(extension_type)
        offset += 4 + extension_length
    return cipher_suites, extensions


class ClientHelloCapture:
    # A listener that reads the first TLS record a client sends and closes the connection; it never answers, so
    # the page's load fails, which is fine: the handshake's first message is all the test wants.
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(8)
        self.port = self.socket.getsockname()[1]
        self.record = None
        self.captured = threading.Event()
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                connection, _ = self.socket.accept()
            except OSError:
                return
            try:
                connection.settimeout(5)
                header = connection.recv(5, socket.MSG_WAITALL)
                if len(header) == 5 and header[0] == 22:
                    (length,) = struct.unpack("!H", header[3:5])
                    body = connection.recv(length, socket.MSG_WAITALL)
                    if self.record is None:
                        self.record = header + body
                        self.captured.set()
            except OSError:
                pass
            finally:
                connection.close()

    def close(self):
        self.socket.close()


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def capture_for_preset(ladybird_binary, profile, preset):
    capture = ClientHelloCapture()
    process = subprocess.Popen(
        [
            ladybird_binary,
            "--headless=manual",
            f"--profile-path={profile}",
            f"--user-agent-preset={preset}",
            f"https://127.0.0.1:{capture.port}/",
        ],
        stderr=subprocess.DEVNULL,
    )
    try:
        assert capture.captured.wait(TIMEOUT_SECONDS), f"No ClientHello arrived for the {preset} preset"
    finally:
        stop(process)
        capture.close()
    return parse_client_hello(capture.record)


def test_tls_handshake_follows_user_agent_preset(ladybird_binary, profile):
    for preset, expected in EXPECTATIONS.items():
        cipher_suites, extensions = capture_for_preset(ladybird_binary, profile / preset.replace(" ", "-"), preset)
        offered = [suite for suite in cipher_suites if not is_grease(suite)]
        assert offered == expected["cipher_suites"], (preset, [hex(suite) for suite in offered])
        has_grease = any(is_grease(suite) for suite in cipher_suites) and any(is_grease(e) for e in extensions)
        assert has_grease == expected["grease"], (preset, "GREASE", has_grease)
        missing = expected["extensions"] - set(extensions)
        assert not missing, (preset, "missing extensions", missing, extensions)
    print("PASS: The TLS handshake takes the shape of the browser the User-Agent preset names")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary
    with tempfile.TemporaryDirectory() as directory:
        test_tls_handshake_follows_user_agent_preset(ladybird_binary, Path(directory))


if __name__ == "__main__":
    main()
