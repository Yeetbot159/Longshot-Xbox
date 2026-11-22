"""Exercise guest-image loading when its size, commit, or read fails."""
import argparse
from pathlib import Path
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cc", "gcc"]
ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    text = (ROOT / "host" / "hos_main.c").read_text(encoding="utf-8")
    start = text.index("static int load_guest_image(")
    end = text.index("\n#endif", start)
    function = text[start:end]
    source = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IMAGE_BASE 4
#define GUEST_SPACE 32
#define _ftelli64 ftell
static unsigned char memory[GUEST_SPACE];
#define A64_MEM memory
static uint64_t hos_image_end = IMAGE_BASE + 4;
static int commits, reads, commit_ok = 1, short_read;
static int hos_commit(uint64_t a, uint64_t n) {
    if (a != IMAGE_BASE || n != 4) return 0;
    ++commits;
    return commit_ok;
}
static size_t test_read(void *p, size_t size, size_t count, FILE *f) {
    ++reads;
    return fread(p, size, short_read ? count - 1 : count, f);
}
#define fread test_read
""" + function + r"""
static int run(const char *data, size_t len, int expected, int want_commits, int want_reads) {
    FILE *f = tmpfile();
    if (!f || fwrite(data, 1, len, f) != len) return 1;
    size_t loaded = 0;
    commits = reads = 0;
    memset(memory, 0, sizeof memory);
    int result = load_guest_image(f, "image.bin", &loaded);
    fclose(f);
    if (result != expected || commits != want_commits || reads != want_reads) return 1;
    if (result && (loaded != len || memcmp(memory + IMAGE_BASE, data, len))) return 1;
    return 0;
}
int main(void) {
    if (run("game", 4, 1, 1, 1)) return 1;
    commit_ok = 0;
    if (run("game", 4, 0, 1, 0)) return 2;
    commit_ok = 1;
    if (run("bad", 3, 0, 0, 0)) return 3;
    if (run("012345678901234567890123456789", 30, 0, 0, 0)) return 4;
    short_read = 1;
    if (run("game", 4, 0, 1, 1)) return 5;
    puts("PASS: image loading checks size, commit and read");
    return 0;
}
"""
    with tempfile.TemporaryDirectory(prefix="longshot-image-test-") as tmp:
        src, exe = Path(tmp) / "image.c", Path(tmp) / "image.exe"
        src.write_text(source, encoding="utf-8")
        subprocess.run([args.cc, "-std=c11", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
