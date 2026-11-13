// Windows backend: Cryptography API: Next Generation (bcrypt.dll), part of Windows.

#include "crypto/crypto.hpp"

#include <digzip/error.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <bcrypt.h>

#include <cstdio>
#include <string>

namespace digzip::crypto {

namespace {

void check(NTSTATUS status, const char* what)
{
    if (!BCRYPT_SUCCESS(status)) {
        char code[16];
        std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(status));
        throw Error(ErrorCode::Io, std::string("crypto: ") + what + " failed (" + code + ")");
    }
}

// Algorithm providers are opened once per process.
class Provider {
public:
    Provider(LPCWSTR algorithm, ULONG flags, LPCWSTR chaining_mode = nullptr)
    {
        check(BCryptOpenAlgorithmProvider(&handle_, algorithm, nullptr, flags), "opening an algorithm");
        if (chaining_mode != nullptr) {
            check(BCryptSetProperty(handle_, BCRYPT_CHAINING_MODE,
                                    reinterpret_cast<PUCHAR>(const_cast<LPWSTR>(chaining_mode)),
                                    static_cast<ULONG>((wcslen(chaining_mode) + 1) * sizeof(wchar_t)), 0),
                  "setting the chaining mode");
        }
    }
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    ~Provider() { BCryptCloseAlgorithmProvider(handle_, 0); }

    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_ = nullptr;
};

BCRYPT_ALG_HANDLE aes_ecb()
{
    static const Provider provider(BCRYPT_AES_ALGORITHM, 0, BCRYPT_CHAIN_MODE_ECB);
    return provider.get();
}

BCRYPT_ALG_HANDLE sha256()
{
    static const Provider provider(BCRYPT_SHA256_ALGORITHM, 0);
    return provider.get();
}

BCRYPT_ALG_HANDLE hmac_sha1()
{
    static const Provider provider(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return provider.get();
}

ULONG to_ulong(std::size_t size)
{
    return static_cast<ULONG>(size);
}

} // namespace

struct Aes::Impl {
    BCRYPT_KEY_HANDLE key = nullptr;
};

Aes::Aes(const std::uint8_t* key, std::size_t key_size) : impl_(std::make_unique<Impl>())
{
    check(BCryptGenerateSymmetricKey(aes_ecb(), &impl_->key, nullptr, 0, const_cast<PUCHAR>(key), to_ulong(key_size), 0),
          "creating an AES key");
}

Aes::~Aes()
{
    BCryptDestroyKey(impl_->key);
}

void Aes::encrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks)
{
    ULONG done = 0;
    check(BCryptEncrypt(impl_->key, const_cast<PUCHAR>(in), to_ulong(blocks * 16), nullptr, nullptr, 0, out,
                        to_ulong(blocks * 16), &done, 0),
          "AES encryption");
}

void Aes::decrypt(const std::uint8_t* in, std::uint8_t* out, std::size_t blocks)
{
    ULONG done = 0;
    check(BCryptDecrypt(impl_->key, const_cast<PUCHAR>(in), to_ulong(blocks * 16), nullptr, nullptr, 0, out,
                        to_ulong(blocks * 16), &done, 0),
          "AES decryption");
}

struct Sha256::Impl {
    BCRYPT_HASH_HANDLE hash = nullptr;
};

Sha256::Sha256() : impl_(std::make_unique<Impl>())
{
    check(BCryptCreateHash(sha256(), &impl_->hash, nullptr, 0, nullptr, 0, 0), "creating a SHA-256 hash");
}

Sha256::~Sha256()
{
    BCryptDestroyHash(impl_->hash);
}

void Sha256::update(const std::uint8_t* data, std::size_t size)
{
    check(BCryptHashData(impl_->hash, const_cast<PUCHAR>(data), to_ulong(size), 0), "SHA-256");
}

std::array<std::uint8_t, 32> Sha256::finish()
{
    std::array<std::uint8_t, 32> digest{};
    check(BCryptFinishHash(impl_->hash, digest.data(), to_ulong(digest.size()), 0), "SHA-256");
    return digest;
}

struct HmacSha1::Impl {
    BCRYPT_HASH_HANDLE hash = nullptr;
};

HmacSha1::HmacSha1(const std::uint8_t* key, std::size_t key_size) : impl_(std::make_unique<Impl>())
{
    check(BCryptCreateHash(hmac_sha1(), &impl_->hash, nullptr, 0, const_cast<PUCHAR>(key), to_ulong(key_size), 0),
          "creating an HMAC-SHA1");
}

HmacSha1::~HmacSha1()
{
    BCryptDestroyHash(impl_->hash);
}

void HmacSha1::update(const std::uint8_t* data, std::size_t size)
{
    check(BCryptHashData(impl_->hash, const_cast<PUCHAR>(data), to_ulong(size), 0), "HMAC-SHA1");
}

std::array<std::uint8_t, 20> HmacSha1::finish()
{
    std::array<std::uint8_t, 20> mac{};
    check(BCryptFinishHash(impl_->hash, mac.data(), to_ulong(mac.size()), 0), "HMAC-SHA1");
    return mac;
}

void pbkdf2_hmac_sha1(const std::uint8_t* password, std::size_t password_size, const std::uint8_t* salt,
                      std::size_t salt_size, unsigned iterations, std::uint8_t* out, std::size_t out_size)
{
    check(BCryptDeriveKeyPBKDF2(hmac_sha1(), const_cast<PUCHAR>(password), to_ulong(password_size),
                                const_cast<PUCHAR>(salt), to_ulong(salt_size), iterations, out, to_ulong(out_size), 0),
          "PBKDF2");
}

std::pair<std::string, std::string> backend()
{
    return {"cng", "system"};
}

void wipe(void* data, std::size_t size)
{
    SecureZeroMemory(data, size);
}

} // namespace digzip::crypto
