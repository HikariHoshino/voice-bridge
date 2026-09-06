"""Restore Tcl/Tk files when the host Python install lacks its script library.

The source must be a previously verified VoiceBridge `_internal` directory built
with the same `python313.dll`.  The script refuses to mix different runtimes.
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
from pathlib import Path


FILES = ("_tkinter.pyd", "tcl86t.dll", "tk86t.dll", "zlib1.dll")
DIRECTORIES = ("_tcl_data", "_tk_data", "tcl8")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def repair(source: Path, destination: Path) -> None:
    source_python = source / "python313.dll"
    destination_python = destination / "python313.dll"
    if not source_python.is_file() or not destination_python.is_file():
        raise RuntimeError("缺少 python313.dll，无法验证 Tk 运行库兼容性")
    if digest(source_python) != digest(destination_python):
        raise RuntimeError("新旧 Python 运行库哈希不同，拒绝混用 Tk 二进制文件")

    for name in FILES:
        shutil.copy2(source / name, destination / name)
    for name in DIRECTORIES:
        target = destination / name
        if target.exists():
            shutil.rmtree(target)
        shutil.copytree(source / name, target)

    tkinter_source = Path(sys.base_prefix) / "Lib" / "tkinter"
    tkinter_target = destination / "tkinter"
    if tkinter_target.exists():
        shutil.rmtree(tkinter_target)
    shutil.copytree(tkinter_source, tkinter_target)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    repair(args.source.resolve(), args.destination.resolve())


if __name__ == "__main__":
    main()
