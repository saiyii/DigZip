# Changelog

All notable changes to DigZip are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- French translation of the graphical interface and of the command-line tool, chosen from the
  system language; *Options > Language* in the window and `DIGZIP_LANG` (`en`, `fr`) choose
  another one. Qt's own dialogs are translated too. JSON output stays in English.

### Changed
- The command-line summary says "N entries skipped" instead of "N skipped".
- The help lists RAR 5 among the formats.

## [0.2.0] - 2026-11-13

A graphical interface, and ready-to-run downloads.
Release notes: [docs/releases/0.2.0.md](docs/releases/0.2.0.md).

### Added
- Graphical interface `digzip-gui` (Qt 6 Widgets, `DIGZIP_BUILD_GUI`): open an archive with a
  button, by drag and drop or from the command line; sortable and filterable content list;
  destination folder and policy for existing files; extraction and test on a worker thread with
  progress and cancellation; password prompt, asked again for the entries a wrong password
  could not decrypt; problems listed in the window.
- Release archives of the graphical interface for Windows and macOS, with the Qt libraries
  shipped separately under the LGPL (`packaging/package.sh`).
- Prebuilt command-line binaries for Windows, Linux and macOS attached to each release.

## [0.1.0] - 2026-11-13

First release: the extraction engine, the command-line tool and their safety net.
Release notes: [docs/releases/0.1.0.md](docs/releases/0.1.0.md).

### Formats
- ZIP: Zip64, data descriptors, self-extracting (prepended) archives, CP437 / UTF-8 /
  Info-ZIP Unicode names, NTFS / Unix / DOS times, Unix modes and symbolic links.
  Methods: stored, deflate, Deflate64, bzip2, LZMA, PPMd (variant I), xz, zstd.
- 7z: compressed and encrypted headers, solid and non-solid blocks, empty files and
  directories, Unix modes and symbolic links. Coders: LZMA, LZMA2, PPMd (variant H), BZip2,
  Deflate, Deflate64, Zstandard, Copy; BCJ x86 / ARM / ARMT / ARM64 / PPC / SPARC / IA64 /
  RISC-V, BCJ2 and Delta filters.
- TAR: ustar, GNU long names, pax records, base-256 numbers; plain or compressed with gzip,
  bzip2, xz or zstd.
- RAR 5 (`DIGZIP_WITH_RAR`, on by default): stored and compressed files, solid archives, small and
  large dictionaries, Delta and x86 filters (the ARM filter is implemented but not covered by a test
  archive yet: RAR only applies it on its own), CRC-32 and BLAKE2sp checksums, symbolic and hard
  links, Unix modes, precise times. No RARLAB code is used.
- Single compressed files: `.gz` (all members), `.bz2`, `.xz`, `.zst`, concatenated streams.
- Container parsing, the gzip framing, Deflate64, PPMd, BCJ2, the RAR 5 decoder and BLAKE2sp are
  DigZip's own code;
  zlib, bzip2, liblzma and zstd do the common decompression.

### Encryption
- ZipCrypto, WinZip AES-128/192/256 (AE-1 and AE-2, authentication code checked),
  7z AES-256 and RAR 5 AES-256, both including encrypted file names; RAR checksums in their
  HMAC form are verified.
- Several passwords, tried in order; they never appear in messages and derived keys are wiped.
- Cryptographic primitives from the system: CNG on Windows, CommonCrypto on macOS,
  OpenSSL 3 elsewhere (`DIGZIP_CRYPTO_BACKEND`).

### Security
- Absolute paths, drive letters, `..` and NUL characters are refused; Windows-invalid names
  are fixed; over-long names and over-deep paths are refused.
- Symbolic links must resolve inside the destination and are created last; nothing is written
  through an existing link; hard links may only target files of the same archive.
- Devices, FIFOs and sockets are skipped; setuid, setgid and sticky bits are dropped.
- Limits on total size, entry count, compression ratio, path depth and decoder memory;
  overlapping ZIP entries (zip bombs) refused; header counts checked before any allocation.
- Every file is written to a temporary name and renamed when complete: no partial files.
- Exact size and CRC-32 checked for every entry.

### Library (`DigZip::engine`)
- `detect`, `list`, `extract`, `test`; `Report` with per-entry warnings and errors.
- Options: include / exclude patterns, `strip_components`, overwrite policy (skip, replace,
  rename), passwords, limits, progress callback, cancellation from any thread.
- `version_info()` with program, API and dependency versions. Static or shared library.

### Command line (`digzip`)
- `list`, `extract`, `test`, `--version`, `--help`, with every engine option.
- Passwords from an argument, a file or an environment variable.
- `--json` output, progress bar, clean stop on Ctrl+C, documented exit codes.
- UTF-8 arguments, output and system messages on Windows.

### Quality
- Catch2 suite with valid, hostile and corrupt fixtures, CLI tests run as a subprocess.
- AddressSanitizer + UndefinedBehaviorSanitizer builds, fuzz targets (libFuzzer or a
  replay-and-mutate driver), CI on Windows, macOS and Linux.
- Builds in C++17 and C++20 mode, with MSVC, Clang and GCC; checked on a POSIX system with
  GCC 16 and OpenSSL 4.
