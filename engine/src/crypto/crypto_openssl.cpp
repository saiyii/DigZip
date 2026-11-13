// OpenSSL 3 backend (libcrypto), the system cryptography library on Linux and other Unix systems.

#include "crypto/crypto.hpp"

#include <digzip/error.hpp>

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <climits>
#include <string>

namespace digzip::crypto {

namespace {

void check(int ok, const char* what)
{
    if (ok != 1) {
        throw Error(ErrorCode::Io, std::string("crypto: ") + what + " failed");
    }
}

template <typename T>
T* require(T* pointer, const char* what)
{
    if (pointer == nullptr) {
        throw Error(ErrorCode::Io, std::string("crypto: ") + what + " failed");
    }
    return pointer;
}

int to_int(std::size_t size)
{
    if (size > static_cast<std::size_t>(INT_MAX)) {
        throw Error(ErrorCode::Io, "crypto: buffer too large");
    }
    return static_cast<int>(size);
}

const EVP_CIPHER* aes_ecb(std::size_t key_size)
{
    switch (key_size) {
    case 16:
        return EVP_aes_128_ecb();
    case 24:
        return EVP_aes_192_ecb();
    case 32:
        return EVP_aes_256_ecb();
    default:
        throw Error(ErrorCode::Corrupt, "crypto: invalid AES key size");
    }
}

} // namespace

struct Aes::Impl {
    EVP_CIPHER_CTX* encrypt = nullptr;
    EVP_CIPHER_CTX* decrypt = nullptr;

    ~Impl()
    {
        EVP_CIPHER_CTX_free(encrypt);
        EVP_CIPHER_CTX_free(decrypt);
    }
};

Aes::Aes(const std::uint8_t* key, std::size_t key_size) : impl_(std::make_unique<Impl>())
{
    const EVP_CIPHER* cipher = aes_ecb(key_size);
    impl_->encrypt = require(EVP_CIPHER_CTX_new(), "creating a cipher context");
    impl_->decrypt = require(EVP_CIPHER_CTX_new(), "creating a cipher context");
    check(EVP_EncryptInit_ex(impl_->encrypt, cipher, nullptr, key, nullptr), "creating an AES key");
    check(EVP_DecryptInit_ex(impl_->decrypt, cipher, nullptr, key, nullptr), "creating an AES key");
    EVP_CIPHER_CTX_set_padding(impl_->encrypt, 0);
    EVP_CIPHER_CTX_set_padding(impl_->decrypt, 0);
}

Aes::~Aes() = default;

void Aes::encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks)
{
    int done = 0;
    check(EVP_EncryptUpdate(impl_->encrypt, out, &done, in, to_int(blocks * 16)), "AES encryption");
}

void Aes::decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks)
{
    int done = 0;
    check(EVP_DecryptUpdate(impl_->decrypt, out, &done, in, to_int(blocks * 16)), "AES decryption");
}

struct Sha256::Impl {
    EVP_MD_CTX* ctx = nullptr;

    ~Impl() { EVP_MD_CTX_free(ctx); }
};

Sha256::Sha256() : impl_(std::make_unique<Impl>())
{
    impl_->ctx = require(EVP_MD_CTX_new(), "creating a SHA-256 hash");
    check(EVP_DigestInit_ex(impl_->ctx, EVP_sha256(), nullptr), "creating a SHA-256 hash");
}

Sha256::~Sha256() = default;

void Sha256::update(const std::uint8_t* data, std::size_t size)
{
    check(EVP_DigestUpdate(impl_->ctx, data, size), "SHA-256");
}

std::array<std::uint8_t, 32> Sha256::finish()
{
    std::array<std::uint8_t, 32> digest{};
    check(EVP_DigestFinal_ex(impl_->ctx, digest.data(), nullptr), "SHA-256");
    return digest;
}

struct Hmac::Impl {
    EVP_MAC* mac = nullptr;
    EVP_MAC_CTX* ctx = nullptr;
    std::size_t size = 0;

    ~Impl()
    {
        EVP_MAC_CTX_free(ctx);
        EVP_MAC_free(mac);
    }
};

Hmac::Hmac(Hash hash, const std::uint8_t* key, std::size_t key_size) : impl_(std::make_unique<Impl>())
{
    impl_->size = hash == Hash::Sha1 ? 20 : 32;
    impl_->mac = require(EVP_MAC_fetch(nullptr, "HMAC", nullptr), "creating an HMAC");
    impl_->ctx = require(EVP_MAC_CTX_new(impl_->mac), "creating an HMAC");
    char sha1[] = "SHA1";
    char sha256[] = "SHA256";
    const OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, hash == Hash::Sha1 ? sha1 : sha256, 0),
        OSSL_PARAM_construct_end()};
    check(EVP_MAC_init(impl_->ctx, key, key_size, params), "creating an HMAC");
}

Hmac::~Hmac() = default;

void Hmac::update(const std::uint8_t* data, std::size_t size)
{
    check(EVP_MAC_update(impl_->ctx, data, size), "HMAC");
}

std::vector<std::uint8_t> Hmac::finish()
{
    std::vector<std::uint8_t> mac(impl_->size);
    std::size_t size = 0;
    check(EVP_MAC_final(impl_->ctx, mac.data(), &size, mac.size()), "HMAC");
    return mac;
}

void pbkdf2(Hash hash, const std::uint8_t* password, std::size_t password_size, const std::uint8_t* salt,
            std::size_t salt_size, unsigned iterations, std::uint8_t* out, std::size_t out_size)
{
    check(PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password), to_int(password_size), salt, to_int(salt_size),
                            static_cast<int>(iterations), hash == Hash::Sha1 ? EVP_sha1() : EVP_sha256(),
                            to_int(out_size), out),
          "PBKDF2");
}

std::pair<std::string, std::string> backend()
{
    return {"openssl", OpenSSL_version(OPENSSL_VERSION_STRING)};
}

void wipe(void* data, std::size_t size)
{
    OPENSSL_cleanse(data, size);
}

} // namespace digzip::crypto
