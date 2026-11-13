# DigZip

A fast, multi-format, open source archive extractor written in C++17.
DigZip is first a library (`DigZip::engine`); the command-line tool and the
future graphical interface are thin clients of it.

> **Status:** v0.x, early development. The C++ API may still change; the stable
> C API comes with v1.0. See [CHANGELOG.md](CHANGELOG.md).

## Features

- **Formats:** ZIP (Zip64, self-extracting), 7z, RAR 5, TAR (ustar, GNU, pax) plain or
  compressed with gzip, bzip2, xz or zstd, and single `.gz`, `.bz2`, `.xz`, `.zst` files.
- **Methods:** stored, deflate, Deflate64, bzip2, LZMA, LZMA2, PPMd, xz, zstd,
  BCJ / BCJ2 / ARM64 / Delta filters.
- **Encryption:** ZipCrypto, WinZip AES-128/192/256, 7z AES-256 and RAR 5 AES-256
  (including encrypted file names). Several passwords can be tried in order.
- **Safe by default:** no path ever leaves the destination, links are checked, special
  files are skipped, decompression bombs are stopped, existing files are kept.
- **Scriptable:** JSON output and documented exit codes.
- Container parsing and the rarer codecs (Deflate64, PPMd, BCJ2, RAR 5) are DigZip's own code;
  zlib, bzip2, liblzma and zstd do the common decompression; encryption primitives come
  from the operating system.

## Download

Prebuilt `digzip` binaries for Windows (x64), Linux (x64, glibc 2.35 or later) and macOS
(11 or later, Apple Silicon and Intel) are attached to each
[release](https://github.com/saiyii/DigZip/releases). Each one is a single executable with no
dependency to install; unpack it and run `digzip --help`.

## Command line

```sh
digzip list    archive.zip                 # entries, sizes, dates
digzip extract archive.7z -o out/          # extract into out/
digzip test    backup.tar.zst              # decode everything, verify checksums
digzip x photos.zip --include "*.jpg" --strip 1 --overwrite rename
digzip x secret.7z --password-file passwords.txt
digzip list archive.zip --json             # for scripts
```

`digzip --help` lists every option. Exit codes:

| Code | Meaning |
|---|---|
| 0 | success |
| 1 | success with warnings (entries skipped or renamed) |
| 2 | command line error |
| 3 | input/output error |
| 4 | corrupt or unsupported data |
| 5 | entries refused for security, or a safety limit reached |
| 6 | password missing or wrong |
| 130 | interrupted (Ctrl+C) |

## Library

```cpp
#include <digzip/archive.hpp>

digzip::ExtractOptions options;
options.passwords = {"secret"};
options.overwrite = digzip::Overwrite::Rename;

const digzip::Report report = digzip::extract("archive.7z", "out", options);
for (const digzip::Issue& issue : report.issues) {
    // issue.severity, issue.code, issue.path, issue.message
}
return report.ok() ? 0 : 1;
```

`list()`, `test()` and `detect()` work the same way. `extract()` and `test()` never
throw for archive or file system problems: everything is reported in `Report::issues`.
Progress and cancellation go through `Options::on_progress` and `Options::cancel`.
The public headers are in [engine/include/digzip](engine/include/digzip).

## Security

See [docs/security.md](docs/security.md) for the threat model and every protection.
In short: absolute paths, drive letters and `..` are refused; symbolic links must
resolve inside the destination and are created last; nothing is written through an
existing link; hard links may only target files of the same archive; devices, FIFOs
and sockets are skipped; setuid/setgid bits are dropped; Windows-invalid names are
fixed; entries whose data overlap (zip bombs) are refused; total size, entry count,
compression ratio, path depth and decoder memory are limited (all configurable).
A failed entry never leaves a partial file. Passwords never appear in messages.

## Building

Requirements: CMake 3.21+, a C++17 compiler (MSVC 2022, GCC 9+, Clang 10+),
[vcpkg](https://github.com/microsoft/vcpkg) with the `VCPKG_ROOT`
environment variable pointing to it, and on Linux the OpenSSL 3 development
files (`libssl-dev` or `openssl-devel`). vcpkg builds the codec libraries
(zlib, bzip2, liblzma, zstd) on the first configure, which takes a few minutes.

```sh
cmake --preset dev          # or: release
cmake --build --preset dev
ctest --preset dev
```

The binaries land in `build/<preset>/bin/`.

### Build options

| Option | Default | Effect |
|---|---|---|
| `DIGZIP_BUILD_CLI` | `ON` | Build the `digzip` executable. |
| `DIGZIP_BUILD_TESTS` | `ON` | Build the test suite (pulls Catch2 through vcpkg). |
| `DIGZIP_WITH_RAR` | `ON` | Read RAR 5 archives (DigZip's own decoder, no RARLAB code). |
| `DIGZIP_BUILD_FUZZERS` | `OFF` | Build the fuzz targets (see below). |
| `DIGZIP_LIBFUZZER` | `OFF` | Link the fuzz targets with libFuzzer (Clang). |
| `DIGZIP_ENABLE_SANITIZERS` | `OFF` | AddressSanitizer, plus UndefinedBehaviorSanitizer outside MSVC. |
| `DIGZIP_WARNINGS_AS_ERRORS` | `OFF` | Treat warnings as errors (`ON` in CI). |
| `BUILD_SHARED_LIBS` | `OFF` | Build the engine as a shared library. |
| `DIGZIP_CRYPTO_BACKEND` | per system | `cng` (Windows), `commoncrypto` (macOS) or `openssl`. With `openssl` on a system without it, add the `openssl` vcpkg feature (`-DVCPKG_MANIFEST_FEATURES="tests;openssl"`). |

### Sanitizers and fuzzing

```sh
CC=clang CXX=clang++ cmake --preset asan && cmake --build --preset asan && ctest --preset asan
CC=clang CXX=clang++ cmake --preset fuzz && cmake --build --preset fuzz
build/fuzz/bin/fuzz_archive -max_total_time=600 corpus/ tests/fixtures
```

Three targets live in [fuzz/](fuzz): whole archives (`fuzz_archive`), DigZip's own
decoders (`fuzz_codecs`) and the path rules (`fuzz_paths`). Without libFuzzer they are
built with a small driver that replays inputs and random mutations of them
(`fuzz_archive --mutations 1000 tests/fixtures`); CTest runs them that way.

## Layout

| Path | Content |
|---|---|
| `engine/` | The library. Public headers in `engine/include/digzip/`. Never depends on the CLI or GUI. |
| `cli/` | The `digzip` command-line tool. |
| `tests/` | Catch2 unit tests and CLI tests (run by CTest). |
| `tests/fixtures/` | Test archives, including hostile and corrupt ones, generated by `make_fixtures.py`. |
| `fuzz/` | Fuzz targets. |
| `docs/` | Documentation. |
| `cmake/` | Shared CMake helpers. |

## Privacy

DigZip collects no telemetry and makes no network call.

## License

Dual-licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT License ([LICENSE-MIT](LICENSE-MIT))

at your option. Third-party components and their licenses are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in DigZip shall be dual-licensed as above, without any
additional terms or conditions.
