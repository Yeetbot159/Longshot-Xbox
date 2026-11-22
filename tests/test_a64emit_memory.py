"""Bound and verify native instruction-translation batches on Xbox.

python -I -B tests/test_a64emit_memory.py
"""
import ctypes
import os
import sys
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "nx2"))
import kit  # noqa: E402,F401
import a64emit  # noqa: E402
import insn  # noqa: E402

insn.install()


class NativeTranslator:
    def __init__(self):
        self.batch_sizes = []

    def a64_translate_batch(self, insns, pcs, count, buf, _capacity, offsets, flags):
        self.batch_sizes.append(count)
        ctypes.memmove(buf, b"x" * count, count)
        for i in range(count):
            offsets[i] = i
            flags[i] = i & 1
        offsets[count] = count
        return count


def translate_with(environment, items):
    native = NativeTranslator()
    reads = []
    real_string_at = ctypes.string_at

    def string_at(buf, size):
        reads.append(size)
        return real_string_at(buf, size)

    with mock.patch.object(a64emit, "lib", return_value=native), \
            mock.patch.object(a64emit.ctypes, "string_at", side_effect=string_at), \
            mock.patch.dict(os.environ, {}, clear=False):
        os.environ.pop("LONGSHOT_XBOX", None)
        os.environ.update(environment)
        texts, flags = a64emit.translate(items)
    if environment.get("LONGSHOT_XBOX") and native.batch_sizes != reads:
        raise AssertionError("native output reads must use the populated byte count")
    if texts != ["x"] * len(items) or flags != [i & 1 for i in range(len(items))]:
        raise AssertionError("translation output changed across batches")
    return native.batch_sizes


def main():
    xbox_batches = translate_with({"LONGSHOT_XBOX": "1"}, [(i, i) for i in range(9000)])
    if xbox_batches != [4096, 4096, 808]:
        raise AssertionError("Xbox batches exceeded the memory bound: %r" % xbox_batches)
    desktop_batches = translate_with({}, [(i, i) for i in range(9000)])
    if desktop_batches != [9000]:
        raise AssertionError("desktop batching changed: %r" % desktop_batches)
    print("ok: Xbox translation batches are bounded; desktop batching and exact output are unchanged")


if __name__ == "__main__":
    main()
