"""Bound and verify native instruction-translation batches on Xbox.

python -I -B tests/test_a64emit_memory.py
"""
import ast
import ctypes
import itertools
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


def test_coverage_batches():
    source = (ROOT / "nx2" / "lift.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    function = next(node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name == "unhandled_words")
    namespace = {"itertools": itertools, "a64emit": a64emit}
    exec(compile(ast.Module(body=[function], type_ignores=[]), "lift.py", "exec"), namespace)
    calls = []

    def translate(items):
        calls.append(len(items))
        return ["generated C"] * len(items), [a64emit.UNHANDLED if w % 7 == 0 else 0 for _, w in items]

    keys = {w: 100000 + w for w in range(20000)}
    with mock.patch.object(a64emit, "translate", side_effect=translate):
        bad = namespace["unhandled_words"](keys)
    if calls != [8192, 8192, 3616] or bad != {w for w in keys if w % 7 == 0}:
        raise AssertionError("coverage batching changed the unhandled words: %r" % calls)


def main():
    test_coverage_batches()
    xbox_batches = translate_with({"LONGSHOT_XBOX": "1"}, [(i, i) for i in range(9000)])
    if xbox_batches != [4096, 4096, 808]:
        raise AssertionError("Xbox batches exceeded the memory bound: %r" % xbox_batches)
    desktop_batches = translate_with({}, [(i, i) for i in range(9000)])
    if desktop_batches != [9000]:
        raise AssertionError("desktop batching changed: %r" % desktop_batches)
    print("ok: Xbox translation batches are bounded; desktop batching and exact output are unchanged")


if __name__ == "__main__":
    main()
