"""Check VMA's renderer-wide heap cap without requiring a Vulkan device.

    python -I -B tests/test_vma_memory_limit.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def check_source():
    source = (ROOT / "gpu/vk/vk_vma.cpp").read_text(encoding="utf-8")
    for needle in ('#include "vma_heap_limits.h"', 'env_int("RX_VK_MEMORY_MB", 0)',
                   "vma_heap_rule::split_heap_limits", "ci.pHeapSizeLimit = vma_heap_limits.data()"):
        if needle not in source:
            raise SystemExit("gpu/vk/vk_vma.cpp is missing VMA memory-boundary wiring: " + needle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    check_source()
    with tempfile.TemporaryDirectory(prefix="vma-memory-limit-") as tmp:
        exe = Path(tmp) / "vma-memory-limit"
        subprocess.run(
            [args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests/test_vma_memory_limit.cpp"), "-o", str(exe)],
            check=True,
        )
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
