# Security model

DigZip treats every archive as hostile. An archive may come from anyone, and extracting
it must never write outside the chosen destination, crash, exhaust the machine, or
silently produce something different from what the archive holds.

## Paths

Every entry name is turned into a list of path components before anything is written.

| Input | Result |
|---|---|
| Absolute path (`/etc/passwd`, `\\server\share`) | refused (`security`) |
| Drive letter (`C:\x`, `C:x`, `a:b.txt`) | refused (`security`), on every system |
| Any `..` component | refused (`security`), even when it would stay inside |
| NUL character | refused (`security`) |
| `\` | treated as a separator on every system, so an archive extracts the same everywhere |
| Empty and `.` components | dropped |
| Component longer than 255 characters | refused (`invalid_name`) |
| More than 256 levels (`max_path_depth`) | refused (`limit_exceeded`) |

On Windows only, names the system cannot store are changed and reported as warnings:
`< > : " | ? *` and control characters become `_`, trailing dots and spaces become `_`,
reserved device names (`CON`, `NUL`, `COM1`...) get a `_` appended. Paths longer than
`MAX_PATH` work through the `\\?\` prefix.

## Links and special files

- A symbolic link target must be relative and, resolved from the link's own directory,
  stay inside the destination. Symbolic links are created after every other entry, so no
  later entry can be written through one.
- Directories are created one level at a time; a level that exists as a symbolic link,
  junction or special file stops the entry (`security`). Nothing is ever written through a
  link that existed in the destination before extraction.
- A hard link may only point to a regular file written earlier by the same extraction.
- Devices, FIFOs and sockets are never created (warning).
- Without the Windows symbolic link privilege, a link to a file from the archive is
  replaced by a copy of that file (warning).

## Files and metadata

- Each file is written to a temporary name in its destination directory and renamed when
  complete: an error, a cancellation or a crash never leaves a partial file under the real
  name, and an existing file is only replaced once the new one is complete
  (`Overwrite::Replace`). By default existing files are kept (`Overwrite::Skip`).
- Permissions: only the `rwx` bits are restored (filtered by the umask); setuid, setgid
  and sticky bits, and owners, are never restored.
- Modification times are restored.

## Resource limits

All limits are in `digzip::Limits`, apply to `list`, `test` and `extract`, and can be
changed or disabled (0).

| Limit | Default | Protects against |
|---|---|---|
| `max_total_size` | 64 GiB | filling the disk |
| `max_ratio` | 1000, checked past 64 MiB | decompression bombs |
| `max_entries` | 1,000,000 | millions of tiny entries |
| `max_path_depth` | 256 | deeply nested paths |
| `max_memory` | 1 GiB per decoder | huge LZMA/xz dictionaries, zstd windows, PPMd models |

The limits count decoded bytes, not the sizes the archive claims. In addition:

- ZIP entries whose compressed data overlap (the "overlapping files" zip bomb) are refused.
- Every count read from an archive header is checked against the bytes actually present
  before anything is allocated (7z headers, ZIP central directory, tar pax records,
  gzip names).
- Each ZIP and 7z entry must decode to exactly its declared size; CRC-32 (and the WinZip
  AES authentication code) are verified.

## Passwords

Passwords are given by the caller (or read by the CLI from an argument, a file or an
environment variable). They are tried in order, never logged, never included in error
messages, and keys derived from them are wiped from memory after use. Cryptographic
primitives come from the operating system (CNG, CommonCrypto, OpenSSL).

## Testing

- `tests/fixtures/` holds hostile archives (path traversal, absolute paths, drive letters,
  escaping symbolic and hard links, overlapping entries, bombs, absurd header counts,
  special files, setuid bits) and corrupt ones; the tests check each refusal and that the
  rest of the archive is still extracted.
- The whole suite runs under AddressSanitizer and UndefinedBehaviorSanitizer in CI.
- Fuzz targets cover whole archives, DigZip's own decoders and the path rules; CI runs them
  with libFuzzer on every push.

## Reporting a vulnerability

Please report security issues privately to the maintainers rather than in a public issue.
