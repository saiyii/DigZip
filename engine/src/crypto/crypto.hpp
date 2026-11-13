#pragma once

// Cryptographic primitives, provided by the operating system: CNG on Windows, CommonCrypto on
// macOS, OpenSSL elsewhere (see DIGZIP_CRYPTO_BACKEND). Block cipher modes and archive-specific
// key derivations are built on top of them in DigZip's own code.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace digzip::crypto {

// AES-128/192/256 in ECB mode over whole 16-byte blocks.
class Aes {
public:
    Aes(const std::uint8_t* key, std::size_t key_size);
    Aes(const Aes&) = delete;
    Aes& operator=(const Aes&) = delete;
    ~Aes();

    void encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks);
    void decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Sha256 {
public:
    Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    ~Sha256();

    void update(const std::uint8_t* data, std::size_t size);
    std::array<std::uint8_t, 32> finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class Hash { Sha1, Sha256 };

class Hmac {
public:
    Hmac(Hash hash, const std::uint8_t* key, std::size_t key_size);
    Hmac(const Hmac&) = delete;
    Hmac& operator=(const Hmac&) = delete;
    ~Hmac();

    void update(const std::uint8_t* data, std::size_t size);
    std::vector<std::uint8_t> finish(); // 20 bytes for SHA-1, 32 for SHA-256

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

void pbkdf2(Hash hash, const std::uint8_t* password, std::size_t password_size, const std::uint8_t* salt,
            std::size_t salt_size, unsigned iterations, std::uint8_t* out, std::size_t out_size);

// Name and version of the backend, for version reports: {"cng", "system"}, {"openssl", "3.5.1"}...
std::pair<std::string, std::string> backend();

// Overwrites key material so that it does not linger in memory.
void wipe(void* data, std::size_t size);

} // namespace digzip::crypto
