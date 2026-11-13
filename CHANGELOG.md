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
