// macOS backend: CommonCrypto, part of the system (libSystem).

#define __STDC_WANT_LIB_EXT1__ 1 // memset_s, which the compiler may not optimize away
#include <string.h>

#include "crypto/crypto.hpp"

#include <digzip/error.hpp>

#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>

#include <string>

namespace digzip::crypto {

namespace {

void check(CCCryptorStatus status, const char* what)
{
    if (status != kCCSuccess) {
        throw Error(ErrorCode::Io, std::string("crypto: ") + what + " failed (" + std::to_string(status) + ")");
    }
}

} // namespace

struct Aes::Impl {
    CCCryptorRef encrypt = nullptr;
    CCCryptorRef decrypt = nullptr;

    ~Impl()
    {
        if (encrypt != nullptr) {
            CCCryptorRelease(encrypt);
        }
        if (decrypt != nullptr) {
            CCCryptorRelease(decrypt);
        }
    }
};

Aes::Aes(const std::uint8_t* key, std::size_t key_size) : impl_(std::make_unique<Impl>())
{
    check(CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key, key_size, nullptr, &impl_->encrypt),
          "creating an AES key");
    check(CCCryptorCreate(kCCDecrypt, kCCAlgorithmAES, kCCOptionECBMode, key, key_size, nullptr, &impl_->decrypt),
          "creating an AES key");
}

Aes::~Aes() = default;

void Aes::encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks)
{
    std::size_t done = 0;
    check(CCCryptorUpdate(impl_->encrypt, in, blocks * 16, out, blocks * 16, &done), "AES encryption");
}

void Aes::decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks)
{
    std::size_t done = 0;
    check(CCCryptorUpdate(impl_->decrypt, in, blocks * 16, out, blocks * 16, &done), "AES decryption");
}

struct Sha256::Impl {
    CC_SHA256_CTX ctx;
};

Sha256::Sha256() : impl_(std::make_unique<Impl>())
{
    CC_SHA256_Init(&impl_->ctx);
}

Sha256::~Sha256() = default;

void Sha256::update(const std::uint8_t* data, std::size_t size)
{
    while (size > 0) {
        const auto chunk = static_cast<CC_LONG>(size > 0x40000000 ? 0x40000000 : size);
        CC_SHA256_Update(&impl_->ctx, data, chunk);
        data += chunk;
        size -= chunk;
    }
}

std::array<std::uint8_t, 32> Sha256::finish()
{
    std::array<std::uint8_t, 32> digest{};
    CC_SHA256_Final(digest.data(), &impl_->ctx);
    return digest;
}

struct Hmac::Impl {
    CCHmacContext ctx;
    std::size_t size = 0;
};

Hmac::Hmac(Hash hash, const std::uint8_t* key, std::size_t key_size) : impl_(std::make_unique<Impl>())
{
    impl_->size = hash == Hash::Sha1 ? 20 : 32;
    CCHmacInit(&impl_->ctx, hash == Hash::Sha1 ? kCCHmacAlgSHA1 : kCCHmacAlgSHA256, key, key_size);
}

Hmac::~Hmac() = default;

void Hmac::update(const std::uint8_t* data, std::size_t size)
{
    CCHmacUpdate(&impl_->ctx, data, size);
}

std::vector<std::uint8_t> Hmac::finish()
{
    std::vector<std::uint8_t> mac(impl_->size);
    CCHmacFinal(&impl_->ctx, mac.data());
    return mac;
}

void pbkdf2(Hash hash, const std::uint8_t* password, std::size_t password_size, const std::uint8_t* salt,
            std::size_t salt_size, unsigned iterations, std::uint8_t* out, std::size_t out_size)
{
    check(CCKeyDerivationPBKDF(kCCPBKDF2, reinterpret_cast<const char*>(password), password_size, salt, salt_size,
                               hash == Hash::Sha1 ? kCCPRFHmacAlgSHA1 : kCCPRFHmacAlgSHA256, iterations, out,
                               out_size),
          "PBKDF2");
}

std::pair<std::string, std::string> backend()
{
    return {"commoncrypto", "system"};
}

void wipe(void* data, std::size_t size)
{
    memset_s(data, size, 0, size);
}

} // namespace digzip::crypto
