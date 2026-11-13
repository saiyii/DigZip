"""Generates the test archives in this directory.

Run with Python 3.14+ (for zstd support):  python make_fixtures.py
Some fixtures are produced by 7-Zip (set DIGZIP_7Z if 7z is not in the default location);
they are kept as committed when 7-Zip is missing.
The outputs are committed, so building and running the tests never needs Python or 7-Zip.
"""

import bz2
import gzip
import io
import lzma
import os
import struct
import subprocess
import tarfile
import tempfile
import zipfile
import zlib
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


# ---------------------------------------------------------------------------
# ZIP

SEVEN_ZIP = os.environ.get("DIGZIP_7Z", r"C:\Program Files\7-Zip\7z.exe")
DOS_TIME = (2023, 11, 14, 22, 13, 20)


def extended_timestamp(mtime=MTIME):
    """Info-ZIP extended timestamp extra field (0x5455), modification time only."""
    return struct.pack("<HHBi", 0x5455, 5, 1, mtime)


def zip_basic(compression):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression) as z:
        def add(name, data, mode=0o644, kind=0o100000):
            info = zipfile.ZipInfo(name, DOS_TIME)
            info.create_system = 3
            info.external_attr = (kind | mode) << 16
            info.extra = extended_timestamp()
            info.compress_type = compression
            z.writestr(info, data)
        info = zipfile.ZipInfo("data/empty_dir/", DOS_TIME)
        info.create_system = 3
        info.external_attr = (0o040755 << 16) | 0x10
        z.writestr(info, b"")
        for name, data in FILES.items():
            add(name, data)
        add("bin/run.sh", b"#!/bin/sh\necho ok\n", mode=0o755)
        add("links/hello-link", b"../hello.txt", mode=0o777, kind=0o120000)
    return buffer.getvalue()


def dos_datetime(t=DOS_TIME):
    year, month, day, hour, minute, second = t
    return (hour << 11) | (minute << 5) | (second // 2), ((year - 1980) << 9) | (month << 5) | day


class ZipEntry:
    def __init__(self, name, data, method=0, payload=None, flags=0, extra=b"", mode=None, crc=None, size=None):
        self.name = name if isinstance(name, bytes) else name.encode("utf-8")
        self.data = data
        self.method = method
        self.payload = data if payload is None else payload
        self.flags = flags
        self.extra = extra
        self.mode = mode
        self.crc = zlib.crc32(data) if crc is None else crc
        self.size = len(data) if size is None else size


def build_zip(entries, comment=b"", duplicate_first=False, zip64=False):
    """Minimal ZIP writer giving full control over every field, for crafted test cases."""
    time, date = dos_datetime()
    out = bytearray()
    central = bytearray()
    records = []
    for e in entries:
        offset = len(out)
        out += struct.pack("<IHHHHHIIIHH", 0x04034B50, 20, e.flags, e.method, time, date,
                           e.crc, len(e.payload), e.size, len(e.name), len(e.extra))
        out += e.name + e.extra + e.payload
        records.append((e, offset))
    if duplicate_first:
        records.append(records[0])
    for e, offset in records:
        made_by = 0x031E if e.mode is not None else 0x0014
        external = (e.mode << 16) if e.mode is not None else 0
        central += struct.pack("<IHHHHHHIIIHHHHHII", 0x02014B50, made_by, 20, e.flags, e.method, time, date,
                               e.crc, len(e.payload), e.size, len(e.name), len(e.extra), 0, 0, 0, external, offset)
        central += e.name + e.extra
    cd_offset = len(out)
    out += central
    count = len(records)
    if zip64:
        eocd64 = len(out)
        out += struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, count, count, len(central), cd_offset)
        out += struct.pack("<IIQI", 0x07064B50, 0, eocd64, 1)
        out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF, 0xFFFFFFFF, 0xFFFFFFFF, len(comment))
    else:
        out += struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, count, count, len(central), cd_offset, len(comment))
    return bytes(out + comment)


def deflate_raw(data):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(data) + c.flush()


def seven_zip(archive, files, *switches):
    """Builds `archive` with 7-Zip from {name: bytes}. Returns False when 7-Zip is unavailable."""
    if not os.path.exists(SEVEN_ZIP):
        print("7-Zip not found, keeping the committed", archive)
        return False
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in files.items():
            path = os.path.join(tmp, *name.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            os.utime(path, (MTIME, MTIME))
        target = os.path.join(HERE, archive)
        if os.path.exists(target):
            os.remove(target)
        subprocess.run([SEVEN_ZIP, "a", *switches, target, *files.keys()], cwd=tmp, check=True,
                       stdout=subprocess.DEVNULL)
    return True


def make_zip_fixtures():
    for suffix, method in [("stored", zipfile.ZIP_STORED), ("deflate", zipfile.ZIP_DEFLATED),
                           ("bzip2", zipfile.ZIP_BZIP2), ("lzma", zipfile.ZIP_LZMA),
                           ("zstd", zipfile.ZIP_ZSTANDARD)]:
        write(f"basic-{suffix}.zip", zip_basic(method))

    seven_zip("basic-deflate64.zip", FILES, "-tzip", "-mm=Deflate64", "-mx=9")
    seven_zip("big-deflate64.zip", {"big.bin": lcg_bytes(50_000) * 3 + bytes(100_000)}, "-tzip", "-mm=Deflate64")

    hello = FILES["hello.txt"]
    write("xz-method.zip", build_zip([ZipEntry("hello.txt", hello, method=95, payload=lzma.compress(hello))]))

    # Data descriptors: zipfile uses them when the output is not seekable.
    class Unseekable(io.RawIOBase):
        def __init__(self):
            self.data = bytearray()

        def writable(self):
            return True

        def write(self, b):
            self.data += b
            return len(b)

    sink = Unseekable()
    with zipfile.ZipFile(sink, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in FILES.items():
            with z.open(name, "w") as f:
                f.write(data)
    write("data-descriptor.zip", bytes(sink.data))

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as z:
        with z.open("hello.txt", "w", force_zip64=True) as f:
            f.write(hello)
    write("zip64-entry.zip", buffer.getvalue())
    write("zip64-eocd.zip", build_zip([ZipEntry("hello.txt", hello)], zip64=True))

    write("sfx.zip", b"MZ" + bytes(998) + zip_basic(zipfile.ZIP_DEFLATED))

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as z:
        z.comment = b"an archive comment"
        z.writestr("hello.txt", hello)
    write("comment.zip", buffer.getvalue())
    buffer = io.BytesIO()
    zipfile.ZipFile(buffer, "w").close()
    write("empty.zip", buffer.getvalue())

    # Names: legacy CP437 bytes, and the Info-ZIP Unicode Path extra field (0x7075).
    cp437 = "caf\u00e9-\u00fc.txt".encode("cp437")
    unicode_path = "\u00e9t\u00e9 \u2615.txt".encode("utf-8")
    raw = b"ete ?.txt"
    unicode_extra = struct.pack("<HHBI", 0x7075, 5 + len(unicode_path), 1, zlib.crc32(raw)) + unicode_path
    write("names.zip", build_zip([
        ZipEntry(cp437, b"cp437\n"),
        ZipEntry(raw, b"unicode extra\n", extra=unicode_extra),
        ZipEntry("utf8-\u00e9.txt", b"utf8\n", flags=0x800),
    ]))

    # Hostile and corrupt archives.
    write("evil-traversal.zip", build_zip([ZipEntry("../evil.txt", b"evil\n"), ZipEntry("safe.txt", b"safe\n")]))
    write("evil-absolute.zip", build_zip([ZipEntry("/tmp/digzip-evil.txt", b"evil\n"),
                                          ZipEntry("C:\\digzip-evil.txt", b"evil\n"),
                                          ZipEntry("safe.txt", b"safe\n")]))
    write("evil-symlink.zip", build_zip([ZipEntry("up", b"../../outside", mode=0o120777),
                                         ZipEntry("safe.txt", b"safe\n")]))
    write("evil-overlap.zip", build_zip([ZipEntry("a.txt", b"x" * 100)], duplicate_first=True))
    write("crc-mismatch.zip", build_zip([ZipEntry("bad.txt", b"payload\n", crc=0x12345678),
                                         ZipEntry("good.txt", b"fine\n")]))
    big = b"A" * 10_000
    write("size-lie.zip", build_zip([ZipEntry("lie.txt", big, method=8, payload=deflate_raw(big), size=100),
                                     ZipEntry("good.txt", b"fine\n")]))
    write("encrypted-flag.zip", build_zip([ZipEntry("secret.txt", b"xxxxxxxxxxxxxxxx", flags=1)]))
    write("unknown-method.zip", build_zip([ZipEntry("odd.bin", b"data", method=77),
                                           ZipEntry("good.txt", b"fine\n")]))
    zeros = bytes(32 << 20)
    write("bomb.zip", build_zip([ZipEntry("zeros.bin", zeros, method=8, payload=deflate_raw(zeros))]))
    basic = zip_basic(zipfile.ZIP_DEFLATED)
    write("truncated.zip", basic[: len(basic) // 2])
    corrupt = bytearray(basic)
    start = corrupt.find(b"data/random.bin") + len("data/random.bin") + 9
    corrupt[start:start + 64] = bytes(64)
    write("corrupt-data.zip", bytes(corrupt))

# ---------------------------------------------------------------------------
# 7z


def x86_like(count):
    """Data with many CALL/JMP opcodes, so that BCJ and BCJ2 have work to do."""
    data = bytearray(lcg_bytes(count, seed=99))
    for i in range(0, count - 5, 23):
        data[i] = 0xE8 if i % 2 else 0xE9
    return bytes(data)


def sz_number(n):
    """7z variable-length number."""
    if n < 0x80:
        return bytes([n])
    for extra in range(1, 9):
        if extra == 8 or n < (1 << (8 * extra + 7 - extra)):
            first = (0xFF << (8 - extra)) & 0xFF
            if extra < 8:
                first |= n >> (8 * extra)
            return bytes([first]) + (n & ((1 << (8 * extra)) - 1)).to_bytes(extra, "little")


def build_7z(files, method_id=b"\x00"):
    """Minimal 7z writer: one Copy folder per file, plain header. For crafted test cases only.

    `files` is a list of (name, data, attributes or None).
    """
    streams = [(name, data, attrs) for name, data, attrs in files]
    packed = b"".join(data for _, data, _ in streams)
    h = bytearray([0x01, 0x04])  # Header, MainStreamsInfo
    h += b"\x06" + sz_number(0) + sz_number(len(streams)) + b"\x09"
    for _, data, _ in streams:
        h += sz_number(len(data))
    h += b"\x00"
    h += b"\x07\x0B" + sz_number(len(streams)) + b"\x00"
    for _ in streams:
        h += b"\x01" + bytes([len(method_id)]) + method_id
    h += b"\x0C"
    for _, data, _ in streams:
        h += sz_number(len(data))
    h += b"\x00"
    h += b"\x08\x0A\x01"
    for _, data, _ in streams:
        h += struct.pack("<I", zlib.crc32(data))
    h += b"\x00\x00"
    h += b"\x05" + sz_number(len(streams))
    names = b"\x00" + b"".join(name.encode("utf-16-le") + b"\x00\x00" for name, _, _ in streams)
    h += b"\x11" + sz_number(len(names)) + names
    if any(attrs is not None for _, _, attrs in streams):
        attributes = b"\x01\x00" + b"".join(struct.pack("<I", attrs or 0) for _, _, attrs in streams)
        h += b"\x15" + sz_number(len(attributes)) + attributes
    h += b"\x00\x00"
    start = struct.pack("<QQI", len(packed), len(h), zlib.crc32(h))
    return b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start + packed + bytes(h)


WORDS = ["archive", "zip", "seven", "data", "the", "of", "and", "to", "stream", "model",
         "context", "symbol", "escape", "range", "decoder", "PPMd", "42", "1999", "\n", ","]


def word_text(count, seed=2024):
    """Pseudo-random word salad; tests/support/fixtures.hpp rebuilds the same bytes."""
    out = []
    x = seed
    for _ in range(count):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(WORDS[(x >> 16) % len(WORDS)])
    return " ".join(out).encode("ascii")


def make_7z_fixtures():
    tree = dict(FILES)
    tree["data/x86.bin"] = x86_like(16_000)
    variants = {
        "basic.7z": [],
        "lzma.7z": ["-m0=LZMA"],
        "bzip2.7z": ["-m0=BZip2"],
        "deflate.7z": ["-m0=Deflate"],
        "deflate64.7z": ["-m0=Deflate64"],
        "copy.7z": ["-m0=Copy"],
        "bcj.7z": ["-mf=BCJ"],
        "arm64.7z": ["-mf=ARM64"],
        "delta.7z": ["-mf=Delta:4"],
        "bcj2.7z": ["-m0=BCJ2", "-m1=LZMA:d20", "-m2=LZMA:d20", "-m3=LZMA:d20",
                    "-mb0s0:1", "-mb0s1:2", "-mb0s2:3"],
        "non-solid.7z": ["-ms=off"],
        "plain-header.7z": ["-mhc=off"],
    }
    for name, switches in variants.items():
        seven_zip(name, tree, "-t7z", "-mtm=on", *switches)
    seven_zip("bomb.7z", {"zeros.bin": bytes(32 << 20)}, "-t7z")
    text = {"text.txt": word_text(40_000)}
    seven_zip("ppmd.7z", text, "-t7z", "-m0=PPMd")
    seven_zip("ppmd-o2.7z", text, "-t7z", "-m0=PPMd:o=2")
    seven_zip("ppmd-o32.7z", text, "-t7z", "-m0=PPMd:o=32:mem=4m")
    seven_zip("ppmd-small-memory.7z", text, "-t7z", "-m0=PPMd:o=16:mem=64k")
    seven_zip("ppmd.zip", text, "-tzip", "-mm=PPMd")
    seven_zip("ppmd-o2.zip", text, "-tzip", "-mm=PPMd:o=2")
    seven_zip("ppmd-o16-1m.zip", text, "-tzip", "-mm=PPMd:o=16:mem=1m")
    seven_zip("ppmd-o4-1m.zip", text, "-tzip", "-mm=PPMd:o=4:mem=1m")
    seven_zip("ppmd-cutoff.zip", text, "-tzip", "-mm=PPMd", "-mx=9")
    seven_zip("ppmd-cutoff-1m.zip", text, "-tzip", "-mm=PPMd:o=12:mem=1m", "-mx=9")
    # Encrypted archives. The non-ASCII password checks the UTF-8 / UTF-16 handling.
    secret = {"secret.txt": b"top secret\n", "notes.txt": word_text(3000), "data/random.bin": FILES["data/random.bin"]}
    seven_zip("zipcrypto.zip", secret, "-tzip", "-psecret", "-mem=ZipCrypto")
    seven_zip("zipcrypto-stored.zip", secret, "-tzip", "-psecret", "-mem=ZipCrypto", "-mx=0")
    for bits in (128, 192, 256):
        seven_zip(f"aes{bits}.zip", secret, "-tzip", "-psecret", f"-mem=AES{bits}")
    seven_zip("aes256-stored.zip", secret, "-tzip", "-psecret", "-mem=AES256", "-mx=0")
    seven_zip("encrypted.7z", secret, "-t7z", "-psecret")
    seven_zip("encrypted-header.7z", secret, "-t7z", "-psecret", "-mhe=on")
    seven_zip("encrypted-unicode.7z", secret, "-t7z", "-ppässwörd ☕", "-mhe=on")
    seven_zip("encrypted-non-solid.7z", secret, "-t7z", "-psecret", "-ms=off")
    seven_zip("encrypted-copy.7z", secret, "-t7z", "-psecret", "-m0=Copy")
    seven_zip("ppmd-binary.7z", {"data/random.bin": FILES["data/random.bin"], "hello.txt": FILES["hello.txt"]},
              "-t7z", "-m0=PPMd")

    write("empty.7z", build_7z([]))
    write("crafted.7z", build_7z([("a.txt", b"first\n", None), ("b/c.txt", b"second\n", None)]))
    write("evil-traversal.7z", build_7z([("..\\evil.txt", b"evil\n", None), ("safe.txt", b"safe\n", None)]))
    write("evil-absolute.7z", build_7z([("/tmp/digzip-evil.txt", b"evil\n", None), ("safe.txt", b"safe\n", None)]))
    unix_link = 0x8000 | (0o120777 << 16)
    write("evil-symlink.7z", build_7z([("up", b"../../outside", unix_link), ("safe.txt", b"safe\n", None)]))
    write("unknown-method.7z", build_7z([("odd.bin", b"data", None)], method_id=b"\x7F\x7F"))

    basic = open(os.path.join(HERE, "basic.7z"), "rb").read()
    write("truncated.7z", basic[: len(basic) // 2])
    damaged = bytearray(basic)
    damaged[100] ^= 0xFF
    write("corrupt-data.7z", bytes(damaged))
    bad_header = bytearray(basic)
    bad_header[-5] ^= 0xFF
    write("bad-header-crc.7z", bytes(bad_header))
    non_solid = bytearray(open(os.path.join(HERE, "non-solid.7z"), "rb").read())
    non_solid[40] ^= 0xFF
    write("corrupt-non-solid.7z", bytes(non_solid))

def make_hostile_header_fixtures():
    """Headers announcing absurd counts or sizes: they must be refused without allocating."""
    # ZIP: the end record claims 65535 entries in a 46-byte central directory.
    zip_bytes = bytearray(build_zip([ZipEntry("a.txt", b"a")]))
    end = zip_bytes.rfind(b"PK\x05\x06")
    struct.pack_into("<HH", zip_bytes, end + 8, 0xFFFF, 0xFFFF)
    write("hostile-zip-count.zip", bytes(zip_bytes))

    # 7z: a plain header announcing 2^40 files.
    header = b"\x01\x05" + sz_number(1 << 40) + b"\x00\x00"
    start = struct.pack("<QQI", 0, len(header), zlib.crc32(header))
    write("hostile-7z-count.7z",
          b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack("<I", zlib.crc32(start)) + start + header)

    # TAR: a pax extended header claiming 8 GiB of records.
    info = tarfile.TarInfo("././@PaxHeader")
    info.type = tarfile.XHDTYPE
    info.size = 8 << 30
    block = bytearray(info.tobuf(tarfile.GNU_FORMAT))
    write("hostile-tar-pax.tar", bytes(block) + bytes(1024))

    # gzip: a file name field longer than any real one.
    write("hostile-gz-name.gz", b"\x1f\x8b\x08\x08\x00\x00\x00\x00\x00\xff" + b"n" * 20000 + b"\x00")

# ---------------------------------------------------------------------------
# RAR 5

RAR = os.environ.get("DIGZIP_RAR", r"C:\Program Files\WinRAR\Rar.exe")


def rar(archive, files, *switches, directories=()):
    """Builds `archive` with RARLAB's rar from {name: bytes}. Keeps the committed file when rar is missing."""
    if not os.path.exists(RAR):
        print("rar not found, keeping the committed", archive)
        return False
    with tempfile.TemporaryDirectory() as tmp:
        for name, data in files.items():
            path = os.path.join(tmp, *name.split("/"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            os.utime(path, (MTIME, MTIME))
        for name in directories:
            os.makedirs(os.path.join(tmp, *name.split("/")), exist_ok=True)
        target = os.path.join(HERE, archive)
        stem = archive[: -len(".rar")]
        for old in os.listdir(HERE):
            if old == archive or (old.startswith(stem + ".part") and old.endswith(".rar")):
                os.remove(os.path.join(HERE, old))
        names = list(files.keys()) + list(directories)
        subprocess.run([RAR, "a", "-idq", *switches, target, *names],
                       cwd=tmp, check=True, stdout=subprocess.DEVNULL)
    return True


def rar_vint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def rar_block(kind, body, extra=b"", data=b"", flags=0):
    flags |= (0x01 if extra else 0) | (0x02 if data else 0)
    fields = rar_vint(kind) + rar_vint(flags)
    if extra:
        fields += rar_vint(len(extra))
    if data:
        fields += rar_vint(len(data))
    fields += body + extra
    size = rar_vint(len(fields))
    return struct.pack("<I", zlib.crc32(size + fields)) + size + fields + data


def rar_file(name, data, unix_mode=None, redirect=None):
    """A stored file block; `redirect` = (type, target) adds a redirection (link) record."""
    name = name.encode("utf-8")
    host, attributes = (1, unix_mode) if unix_mode is not None else (0, 0x20)
    body = (rar_vint(0x04) + rar_vint(len(data)) + rar_vint(attributes) + struct.pack("<I", zlib.crc32(data))
            + rar_vint(0) + rar_vint(host) + rar_vint(len(name)) + name)
    extra = b""
    if redirect:
        kind, target = redirect
        record = rar_vint(5) + rar_vint(kind) + rar_vint(0) + rar_vint(len(target)) + target.encode("utf-8")
        extra = rar_vint(len(record)) + record
    return rar_block(2, body, extra, data)


def build_rar(files):
    return (b"Rar!\x1a\x07\x01\x00" + rar_block(1, rar_vint(0)) + b"".join(files) + rar_block(5, rar_vint(0)))


def make_rar_fixtures():
    big_text = word_text(120_000)
    rar("basic.rar", FILES, directories=["data/empty_dir"])
    rar("stored.rar", FILES, "-m0")
    rar("best-solid.rar", FILES, "-m5", "-s")
    rar("solid.rar", {f"part{i}.txt": word_text(4000, seed=i) for i in range(6)}, "-s", "-m3")
    rar("large.rar", {"large.txt": big_text}, "-m3")
    rar("small-dict.rar", {"large.txt": big_text, "random.bin": lcg_bytes(30_000)}, "-md128k", "-m3")
    rar("x86.rar", {"x86.bin": x86_like(60_000)}, "-mce+", "-m3")
    rar("delta.rar", {"samples.bin": bytes((i * 7 + (i // 4) * 3) & 0xFF for i in range(120_000))}, "-mc4d+", "-m3")
    rar("blake2.rar", FILES, "-htb")
    rar("encrypted.rar", {"secret.txt": b"top secret\n", "notes.txt": word_text(3000)}, "-psecret")
    rar("encrypted-headers.rar", {"secret.txt": b"top secret\n", "notes.txt": word_text(3000)}, "-hpsecret")
    rar("volumes.rar", {"random.bin": lcg_bytes(60_000)}, "-v20k", "-m0")

    hello = FILES["hello.txt"]
    write("crafted.rar", build_rar([rar_file("a.txt", b"first\n"), rar_file("b/c.txt", b"second\n")]))
    write("evil-traversal.rar", build_rar([rar_file("../evil.txt", b"evil\n"), rar_file("safe.txt", b"safe\n")]))
    write("evil-absolute.rar", build_rar([rar_file("/tmp/digzip-evil.txt", b"evil\n"),
                                          rar_file("safe.txt", b"safe\n")]))
    write("evil-symlink.rar", build_rar([rar_file("up", b"", unix_mode=0o120777, redirect=(1, "../../outside")),
                                         rar_file("safe.txt", b"safe\n")]))
    write("links.rar", build_rar([rar_file("hello.txt", hello),
                                  rar_file("hello-hard", b"", redirect=(4, "hello.txt")),
                                  rar_file("hello-link", b"", unix_mode=0o120777, redirect=(1, "hello.txt"))]))
    basic = open(os.path.join(HERE, "basic.rar"), "rb").read()
    write("truncated.rar", basic[: len(basic) // 2])
    damaged = bytearray(basic)
    damaged[len(basic) // 2] ^= 0xFF
    write("corrupt-data.rar", bytes(damaged))
    write("rar4-signature.rar", b"Rar!\x1a\x07\x00" + bytes(64))


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

    make_zip_fixtures()
    make_7z_fixtures()
    make_hostile_header_fixtures()
    make_rar_fixtures()


if __name__ == "__main__":
    main()
