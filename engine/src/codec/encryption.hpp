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

// RAR 5: AES-256 key, key for the checksums of encrypted files (HMAC-SHA256), and the 8-byte
// password check value, from the password (UTF-8), a 16-byte salt and 2^log2_count rounds.
struct Rar5Keys {
    Key256 key{};
    Key256 hash_key{};
    std::array<std::uint8_t, 8> check{};
};
Rar5Keys derive_rar5_keys(const std::string& password, const std::uint8_t* salt, unsigned log2_count);

// AES-256-CBC decryption with a 16-byte IV, as a stream or in place.
io::StreamPtr decrypt_aes256_cbc(io::StreamPtr upstream, const Key256& key, const std::uint8_t* iv);
void decrypt_aes256_cbc(const Key256& key, const std::uint8_t* iv, std::uint8_t* data, std::size_t size);

} // namespace digzip::codec
