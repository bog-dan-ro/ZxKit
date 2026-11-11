#!/usr/bin/env python3
# Copyright (c) 2026, BogDan Vatra. SPDX-License-Identifier: GPL-3.0-or-later
"""Compatibility import for the portable skill's shared stdio client."""

from pathlib import Path
from runpy import run_path

Client = run_path(str(Path(__file__).resolve().parent / "skills/zx-mcp/scripts/client.py"))["Client"]
