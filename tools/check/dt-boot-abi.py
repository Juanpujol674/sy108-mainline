#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate the installed boot ABI without extending the standard chosen schema."""
from pathlib import Path
import argparse
import json
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "lib"))
from boot_layout import boot_abi, fdt_props

p = argparse.ArgumentParser(description=__doc__)
p.add_argument("--profile", choices=("main", "recovery"), required=True)
p.add_argument("dtb", type=Path)
a = p.parse_args()
result = boot_abi(fdt_props(a.dtb.read_bytes()), "openwrt" if a.profile == "main" else "recovery")
print(json.dumps({"profile": a.profile, "passed": True, "boot_abi": result}, indent=2))
