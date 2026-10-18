#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run a command in the shared OpenWrt builder."""
import argparse
import os
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent / 'lib'))
from workspace import container

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--offline', action='store_true')
p.add_argument('--root', action='store_true')
p.add_argument('command', nargs=argparse.REMAINDER)
a = p.parse_args()
command = a.command[1:] if a.command[:1] == ['--'] else a.command
argv = container(command or ['bash'], offline=a.offline, root=a.root)
os.execvp(argv[0], argv)
