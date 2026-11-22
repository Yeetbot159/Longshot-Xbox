"""Bounded-memory writer for the Xbox lift; matches a64-kit's generated files."""
import itertools
import os

import a64emit
import a64_lift


def write(lifter, out_dir, chunk_bytes):
    os.makedirs(out_dir, exist_ok=True)
    order = sorted(lifter.funcs)
    fpx = set()
    chunk, size, number, decls, files = [], 0, 0, set(), []

    def save(path, parts, declarations):
        with open(path, "w", newline="\n") as f:
            f.write('#include "a64.h"\n')
            if any("recomp_sj_setjmp(" in part for part in parts):
                f.write("__attribute__((returns_twice)) int recomp_sj_setjmp(void **slot);\n")
            f.writelines("void %s(Ctx *A64_NOESC);\n" % d for d in sorted(declarations))
            f.writelines(parts)

    def flush():
        nonlocal chunk, size, number, decls
        if not chunk:
            return
        path = os.path.join(out_dir, "code_%03d.c" % number)
        save(path, chunk, decls)
        files.append(path)
        chunk, size, number, decls = [], 0, number + 1, set()

    hot = a64_lift.hot_functions()
    hot_chunk = [] if hot and os.environ.get("A64_HOT_CHUNK") else None
    hot_decls, nhot = set(), 0
    next_function = 0
    while next_function < len(order):
        first = next_function
        count = 0
        while next_function < len(order) and (count < 8192 or next_function == first):
            count += len(lifter.funcs[order[next_function]][0])
            next_function += 1
        keys = {}
        for e in order[first:next_function]:
            for pc, (kind, word) in lifter.funcs[e][0].items():
                if kind[0] == "op":
                    keys.setdefault((word, pc) if a64_lift.pc_relative(word) else word, (pc, word))
        texts = {}
        entries = iter(keys.items())
        while True:
            batch = list(itertools.islice(entries, 4096))
            if not batch:
                break
            translated, flags = a64emit.translate([item for _, item in batch])
            for (key, _), text, flag in zip(batch, translated, flags):
                texts[key] = (a64_lift.op_text(text, fpx), flag)
            del translated, flags
        del keys, entries, batch
        for e in order[first:next_function]:
            chunk_bytes.current = e
            text, refs = lifter.emit_function(e, texts)
            if hot and lifter.fname(e) in hot:
                text = a64_lift.hot_text(text)
                nhot += 1
                if hot_chunk is not None:
                    hot_chunk.append(text)
                    hot_decls.update(refs)
                    continue
            decls.update(refs)
            chunk.append(text)
            size += len(text)
            if size >= chunk_bytes:
                flush()
        del texts
        # These vendored memo tables retain translated text as keys, including PC-specific instructions.
        a64_lift._flag_masks.clear()
        a64_lift._reg_writes.clear()

    flush()
    hot_path = os.path.join(out_dir, "code_hot.c")
    if hot_chunk:
        save(hot_path, hot_chunk, hot_decls)
        files.append(hot_path)
    elif os.path.exists(hot_path):
        os.remove(hot_path)
    if hot:
        print("hot functions (A64_HOT_FUNCS): %d of %d listed found%s" %
              (nhot, len(hot), ", in code_hot.c" if hot_chunk else ""))
    with open(os.path.join(out_dir, "a64_fpx.h"), "w", newline="\n") as f:
        f.write(a64_lift.fpx_header(fpx))
        if a64_lift.local_flags():
            f.write(a64_lift.FCMPK_HEADER)
    lifter._write_table(out_dir)
    with open(os.path.join(out_dir, "functions.txt"), "w") as f:
        for e in order:
            f.write("%08x %d\n" % (e, len(lifter.funcs[e][0])))
    return files
