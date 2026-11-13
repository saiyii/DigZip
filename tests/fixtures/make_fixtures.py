"""Generates the test archives in this directory.

Run with Python 3.14+ (for zstd support):  python make_fixtures.py
The outputs are committed, so building and running the tests never needs Python.
"""

import bz2
import gzip
import io
import lzma
import os
import tarfile
from compression import zstd

HERE = os.path.dirname(os.path.abspath(__file__))
MTIME = 1_700_000_000


def lcg_bytes(count, seed=12345):
    """Same generator as tests/support/fixtures.hpp, so C++ tests can rebuild the expected bytes."""
    out = bytearray()
    x = seed
    for _ in range(count):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


FILES = {
    "hello.txt": b"Hello, DigZip!\n",
    "docs/readme.md": b"# DigZip\n" * 50,
    "docs/empty.txt": b"",
    "data/random.bin": lcg_bytes(20000),
    "unicode/caf\u00e9 \u2615.txt": b"unicode\n",
}


def write(name, data):
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(data)


def add_file(tar, name, data, mode=0o644, mtime=MTIME):
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = mode
    info.mtime = mtime
    tar.addfile(info, io.BytesIO(data))


def add_dir(tar, name):
    info = tarfile.TarInfo(name)
    info.type = tarfile.DIRTYPE
    info.mode = 0o755
    info.mtime = MTIME
    tar.addfile(info)


def add_link(tar, name, target, kind=tarfile.SYMTYPE):
    info = tarfile.TarInfo(name)
    info.type = kind
    info.linkname = target
    info.mtime = MTIME
    tar.addfile(info)


def add_special(tar, name, kind):
    info = tarfile.TarInfo(name)
    info.type = kind
    info.mtime = MTIME
    tar.addfile(info)


def make_tar(build, fmt=tarfile.PAX_FORMAT):
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=fmt) as tar:
        build(tar)
    return buffer.getvalue()


def gz(data, name=None):
    buffer = io.BytesIO()
    with gzip.GzipFile(filename=name or "", mode="wb", fileobj=buffer, mtime=0) as f:
        f.write(data)
    return buffer.getvalue()


def basic_tree(tar):
    add_dir(tar, "data/empty_dir")
    for name, data in FILES.items():
        add_file(tar, name, data)
    add_file(tar, "bin/run.sh", b"#!/bin/sh\necho ok\n", mode=0o755)
    add_link(tar, "links/hello-link", "../hello.txt")
    add_link(tar, "links/hello-hard", "hello.txt", tarfile.LNKTYPE)


def main():
    basic = make_tar(basic_tree)
    write("basic.tar", basic)
    write("basic.tar.gz", gz(basic))
    write("basic.tar.bz2", bz2.compress(basic))
    write("basic.tar.xz", lzma.compress(basic))
    write("basic.tar.zst", zstd.compress(basic))

    long_name = "long/" + "n" * 120 + ".txt"
    write("gnu-longname.tar", make_tar(lambda t: add_file(t, long_name, b"long\n"), tarfile.GNU_FORMAT))
    prefixed = "p" * 90 + "/" + "q" * 60 + ".txt"
    write("ustar-prefix.tar", make_tar(lambda t: add_file(t, prefixed, b"prefix\n"), tarfile.USTAR_FORMAT))

    hello = FILES["hello.txt"]
    write("hello.txt.gz", gz(hello, "hello.txt"))
    write("hello.txt.bz2", bz2.compress(hello))
    write("hello.txt.xz", lzma.compress(hello))
    write("hello.txt.zst", zstd.compress(hello))
    write("noname.gz", gz(hello))
    write("multi-member.gz", gz(b"part1\n", "parts.txt") + gz(b"part2\n"))
    write("concat.bz2", bz2.compress(b"part1\n") + bz2.compress(b"part2\n"))
    write("concat.xz", lzma.compress(b"part1\n") + lzma.compress(b"part2\n"))
    write("concat.zst", zstd.compress(b"part1\n") + zstd.compress(b"part2\n"))

    # Hostile archives. Each also holds "safe.txt", which must still be extracted.
    def evil(*names):
        def build(tar):
            for name in names:
                add_file(tar, name, b"evil\n")
            add_file(tar, "safe.txt", b"safe\n")
        return make_tar(build)

    write("evil-traversal.tar", evil("../evil.txt", "a/../../evil2.txt"))
    write("evil-absolute.tar", evil("/tmp/digzip-evil.txt"))
    write("evil-drive.tar", evil("C:/digzip-evil.txt", "C:digzip-evil2.txt", "a:b.txt"))
    write("evil-backslash.tar", evil("..\\digzip-evil.txt", "a\\..\\..\\digzip-evil2.txt"))

    def symlinks(tar):
        add_link(tar, "up", "../..")
        add_link(tar, "abs", "/etc")
        add_link(tar, "sneaky", "a/../../outside")
        add_file(tar, "safe.txt", b"safe\n")
    write("evil-symlink.tar", make_tar(symlinks))

    def write_through(tar):
        add_link(tar, "inside", "real")
        add_file(tar, "inside/file.txt", b"through\n")
    write("evil-write-through.tar", make_tar(write_through))

    def hardlinks(tar):
        add_link(tar, "h-abs", "/etc/passwd", tarfile.LNKTYPE)
        add_link(tar, "h-up", "../outside.txt", tarfile.LNKTYPE)
        add_link(tar, "h-missing", "missing.txt", tarfile.LNKTYPE)
        add_file(tar, "safe.txt", b"safe\n")
    write("evil-hardlink.tar", make_tar(hardlinks))

    def specials(tar):
        add_special(tar, "dev-char", tarfile.CHRTYPE)
        add_special(tar, "dev-block", tarfile.BLKTYPE)
        add_special(tar, "fifo", tarfile.FIFOTYPE)
        add_file(tar, "safe.txt", b"safe\n")
    write("special.tar", make_tar(specials))

    def setuid(tar):
        add_file(tar, "suid.sh", b"#!/bin/sh\n", mode=0o4755)
    write("setuid.tar", make_tar(setuid))

    def windows_names(tar):
        for name in ["con.txt", "AUX", "ab:c.txt", "q?.txt", "trailing. ", "COM1.log", "nul .txt"]:
            add_file(tar, name, b"name\n")
    write("windows-names.tar", make_tar(windows_names))

    write("deep.tar", make_tar(lambda t: add_file(t, "d/" * 300 + "f.txt", b"deep\n")))
    write("long-component.tar", make_tar(lambda t: add_file(t, "x" * 300 + ".txt", b"long\n")))
    write("many-entries.tar", make_tar(lambda t: [add_file(t, f"f{i:03}.txt", b"x") for i in range(50)]))
    zeros = make_tar(lambda t: add_file(t, "zeros.bin", bytes(32 << 20)))
    write("bomb.tar.gz", gz(zeros))
    write("bomb.gz", gz(bytes(32 << 20), "zeros.bin"))

    # Corrupt archives: must fail cleanly, without crashing or leaving partial files.
    broken = bytearray(basic)
    broken[0] ^= 0xFF
    write("corrupt-checksum.tar", bytes(broken))
    targz = gz(basic)
    write("truncated.tar.gz", targz[: len(targz) // 2])
    bad_crc = bytearray(gz(hello, "hello.txt"))
    bad_crc[-8] ^= 0xFF
    write("bad-crc.gz", bytes(bad_crc))
    write("truncated.xz", lzma.compress(lcg_bytes(5000))[:-20])
    write("truncated.zst", zstd.compress(lcg_bytes(5000))[:-20])
    write("truncated.tar", basic[: len(basic) // 2])
    write("garbage.bin", lcg_bytes(4096, seed=7))


if __name__ == "__main__":
    main()
