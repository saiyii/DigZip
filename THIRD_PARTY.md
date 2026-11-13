# Third-party components

DigZip is dual-licensed under MIT and Apache-2.0. Every dependency below is
compatible with both. Versions are pinned by the vcpkg `builtin-baseline` in
`vcpkg.json`; `digzip --version` prints the versions actually linked.

Archive containers (ZIP, TAR, 7z, ...) are parsed by DigZip's own code. Only
the decompression codecs come from external libraries.

## Linked into DigZip

| Component | Used for | License | Notes |
|---|---|---|---|
| zlib | Deflate (ZIP, GZ) | Zlib | |
| bzip2 | BZ2, ZIP method 12 | bzip2 license (BSD-style) | Library only, the `bzip2` tool is not built. |
| liblzma (xz) | XZ, LZMA/LZMA2 (7z, ZIP method 14), BCJ and Delta filters | 0BSD | |
| zstd | ZSTD, ZIP method 93 | BSD-3-Clause (dual with GPL-2.0; BSD is used) | |

## Build and test only (not distributed)

| Component | Used for | License |
|---|---|---|
| Catch2 v3 | Unit tests | BSL-1.0 |
| vcpkg | Dependency builds | MIT. Telemetry is disabled (`-disableMetrics`, `VCPKG_DISABLE_METRICS`). |

## Planned

| Component | Version | Notes |
|---|---|---|
| System cryptography | v0.x phase 3 | AES, SHA, PBKDF2 for encrypted archives: CNG on Windows, CommonCrypto on macOS, OpenSSL 3 (Apache-2.0) on Linux. |

## Explicitly excluded

| Component | Reason |
|---|---|
| UnRAR (RARLAB) | Freeware licence, not OSI-approved, forbids re-creating the RAR compressor. Can only ever be an opt-in module, never on by default. |
