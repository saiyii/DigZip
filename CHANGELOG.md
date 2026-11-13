# Changelog

All notable changes to DigZip are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Repository skeleton: CMake build with presets, vcpkg manifest, dual MIT / Apache-2.0 license.
- Engine library (`DigZip::engine`) linked against the zlib, bzip2, liblzma and zstd codecs.
  Archive containers will be parsed by DigZip's own code.
- `digzip::version_info()` reporting program, API and dependency versions.
- Minimal `digzip --version` command.
- Catch2 test suite and GitHub Actions CI on Windows, macOS and Linux.
- Engine API: `detect`, `list`, `extract` and `test`, with a per-entry `Report` of warnings and errors.
- Formats: TAR (ustar, GNU long names, pax) plain or compressed with gzip, bzip2, xz or zstd,
  and single compressed files (`.gz`, `.bz2`, `.xz`, `.zst`, concatenated streams included).
  Container parsing and the gzip framing are DigZip's own code.
- Security: paths with `..`, absolute paths and drive letters are refused; symbolic links must
  resolve inside the destination and are created last; nothing is written through an existing
  link; hard links may only target files from the same archive; devices, FIFOs and sockets are
  skipped; setuid/setgid/sticky bits are dropped; Windows-invalid names are fixed (reserved device
  names, forbidden characters, trailing dots), over-long names refused.
- Anti-bomb limits (total size, entry count, compression ratio, path depth, decoder memory),
  all configurable.
- Files are written to a temporary name and renamed when complete: an error never leaves a
  partial file, and existing files are never overwritten.
- ZIP: in-house reader with Zip64, data descriptors, self-extracting (prepended) archives,
  CP437 / UTF-8 / Info-ZIP Unicode names, NTFS / Unix / DOS times, Unix modes and symbolic
  links. Methods: stored, deflate, Deflate64 (DigZip's own decoder), bzip2, LZMA, zstd, xz.
  Every entry is checked for exact size and CRC-32; overlapping entries (a zip bomb technique)
  are refused; a broken entry does not stop the others.
