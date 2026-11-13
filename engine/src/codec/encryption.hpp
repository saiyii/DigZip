#pragma once

#include "io/stream.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace digzip::codec {

// Traditional PKWARE encryption ("ZipCrypto"). Reads the 12-byte header and throws
// Error(WrongPassword) if its check byte does not match `check`.
io::StreamPtr decrypt_zipcrypto(io::StreamPtr upstream, const std::string& password, std::uint8_t check);

// WinZip AES (AE-1 / AE-2), strength 1, 2 or 3 for AES-128, 192 or 256. `packed_size` covers the
// salt, password verifier, encrypted data and authentication code. Throws Error(WrongPassword)
// when the verifier does not match; the authentication code is checked at the end of the data.
io::StreamPtr decrypt_winzip_aes(io::StreamPtr upstream, std::uint64_t packed_size, unsigned strength,
                                 const std::string& password);

using Key256 = std::array<std::uint8_t, 32>;

// 7z AES-256: key derived from the password (UTF-8, used as UTF-16LE) and the coder properties
// (salt, number of SHA-256 rounds).
Key256 derive_7z_key(const std::vector<std::uint8_t>& properties, const std::string& password);

// What the 7z key depends on (rounds and salt, not the IV): equal identities give equal keys.
std::vector<std::uint8_t> sevenzip_key_identity(const std::vector<std::uint8_t>& properties);

// 7z AES-256-CBC decryption with a key from derive_7z_key().
io::StreamPtr decrypt_7z_aes(io::StreamPtr upstream, const std::vector<std::uint8_t>& properties, const Key256& key);

} // namespace digzip::codec
