#!/usr/bin/env python3
"""Validate release identity before downloading SDKs or compiling."""
import os
from pathlib import Path
import re

root = Path(__file__).resolve().parents[2]
match = re.search(r"project\(OKILTVQt VERSION ([0-9]+\.[0-9]+\.[0-9]+)\b",
                  (root / "qt/CMakeLists.txt").read_text())
if not match:
    raise SystemExit("Cannot read the application version from qt/CMakeLists.txt")
version = match[1]
if os.environ.get("GITHUB_EVENT_NAME") == "release":
    expected = f"v{version}"
    if os.environ.get("RELEASE_TAG") != expected:
        raise SystemExit(f"Release tag must be {expected}, matching qt/CMakeLists.txt")
if output := os.environ.get("GITHUB_OUTPUT"):
    with open(output, "a") as stream:
        stream.write(f"version={version}\n")
print(f"Application version: {version}")
