#include "codec/encryption.hpp"
#include "crypto/crypto.hpp"
#include "text.hpp"

#include <digzip/error.hpp>

#include <algorithm>
#include <cstring>

namespace digzip::codec {

namespace {

constexpr std::size_t kBlock = 16;
constexpr std::size_t kChunkBlocks = 4096; // 64 KiB per cipher call

[[noreturn]] void wrong_password()
{
    throw Error(ErrorCode::WrongPassword, "wrong password");
}

// --- ZipCrypto --------------------------------------------------------------------------------

struct CrcTable {
    std::uint32_t values[256];

    CrcTable()
    {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            values[i] = c;
        }
    }
};

std::uint32_t crc_update(std::uint32_t crc, std::uint8_t byte)
{
    static const CrcTable table;
    return table.values[(crc ^ byte) & 0xFF] ^ (crc >> 8);
}

class ZipCryptoStream final : public io::InputStream {
public:
    ZipCryptoStream(io::StreamPtr upstream, const std::string& password, std::uint8_t check)
        : upstream_(std::move(upstream))
    {
        for (const char c : password) {
            update(static_cast<std::uint8_t>(c));
        }
        std::uint8_t header[12];
        upstream_->read_exact(header, sizeof header);
        for (std::uint8_t& b : header) {
            b = decrypt(b);
        }
        if (header[11] != check) {
            wrong_password();
        }
    }

    ~ZipCryptoStream() override { crypto::wipe(keys_, sizeof keys_); }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        const std::size_t n = upstream_->read(data, size);
        for (std::size_t i = 0; i < n; ++i) {
            data[i] = decrypt(data[i]);
        }
        return n;
    }

private:
    void update(std::uint8_t plain)
    {
        keys_[0] = crc_update(keys_[0], plain);
        keys_[1] = (keys_[1] + (keys_[0] & 0xFF)) * 134775813u + 1;
        keys_[2] = crc_update(keys_[2], static_cast<std::uint8_t>(keys_[1] >> 24));
    }

    std::uint8_t decrypt(std::uint8_t cipher)
    {
        const std::uint32_t temp = (keys_[2] | 2) & 0xFFFF;
        const auto plain = static_cast<std::uint8_t>(cipher ^ ((temp * (temp ^ 1)) >> 8));
        update(plain);
        return plain;
    }

    io::StreamPtr upstream_;
    std::uint32_t keys_[3] = {0x12345678, 0x23456789, 0x34567890};
};

// --- WinZip AES -------------------------------------------------------------------------------

class WinZipAesStream final : public io::InputStream {
public:
    WinZipAesStream(io::StreamPtr upstream, std::uint64_t packed_size, unsigned strength, const std::string& password)
        : upstream_(std::move(upstream))
    {
        if (strength < 1 || strength > 3) {
            throw Error(ErrorCode::Unsupported, "zip: unknown AES strength");
        }
        const std::size_t key_size = 8 + 8 * strength;
        const std::size_t salt_size = key_size / 2;
        if (packed_size < salt_size + 2 + kAuthSize) {
            throw Error(ErrorCode::Corrupt, "zip: encrypted entry too short");
        }
        remaining_ = packed_size - salt_size - 2 - kAuthSize;

        std::uint8_t salt[16];
        std::uint8_t verifier[2];
        upstream_->read_exact(salt, salt_size);
        upstream_->read_exact(verifier, sizeof verifier);

        std::uint8_t derived[2 * 32 + 2];
        const std::size_t derived_size = 2 * key_size + 2;
        crypto::pbkdf2(crypto::Hash::Sha1, reinterpret_cast<const std::uint8_t*>(password.data()), password.size(),
                       salt, salt_size, 1000, derived, derived_size);
        const bool match = std::memcmp(derived + 2 * key_size, verifier, 2) == 0;
        if (match) {
            aes_ = std::make_unique<crypto::Aes>(derived, key_size);
            mac_ = std::make_unique<crypto::Hmac>(crypto::Hash::Sha1, derived + key_size, key_size);
        }
        crypto::wipe(derived, sizeof derived);
        if (!match) {
            wrong_password();
        }
    }

    ~WinZipAesStream() override { crypto::wipe(keystream_, sizeof keystream_); }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (remaining_ == 0) {
            return 0;
        }
        const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(size, remaining_));
        const std::size_t n = upstream_->read(data, wanted);
        if (n == 0) {
            throw Error(ErrorCode::Corrupt, "zip: unexpected end of encrypted data");
        }
        remaining_ -= n;
        mac_->update(data, n);
        if (remaining_ == 0) {
            // Checked as soon as the last encrypted byte is read: decoders such as zlib stop at the
            // end of their stream and never ask for more.
            verify();
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (keystream_pos_ == sizeof keystream_) {
                refill();
            }
            data[i] ^= keystream_[keystream_pos_++];
        }
        return n;
    }

private:
    static constexpr std::size_t kAuthSize = 10;

    // CTR mode with a 128-bit little-endian counter starting at 1.
    void refill()
    {
        for (std::size_t b = 0; b < kKeystreamBlocks; ++b) {
            for (std::size_t i = 0; i < kBlock && ++counter_[i] == 0; ++i) {
            }
            std::memcpy(keystream_ + b * kBlock, counter_, kBlock);
        }
        aes_->encrypt(keystream_, keystream_, kKeystreamBlocks);
        keystream_pos_ = 0;
    }

    void verify()
    {
        std::uint8_t stored[kAuthSize];
        upstream_->read_exact(stored, sizeof stored);
        const auto computed = mac_->finish();
        if (std::memcmp(stored, computed.data(), kAuthSize) != 0) {
            throw Error(ErrorCode::Checksum, "zip: authentication code mismatch, the entry is corrupt");
        }
    }

    static constexpr std::size_t kKeystreamBlocks = 256;

    io::StreamPtr upstream_;
    std::uint64_t remaining_ = 0;
    std::unique_ptr<crypto::Aes> aes_;
    std::unique_ptr<crypto::Hmac> mac_;
    std::uint8_t counter_[kBlock] = {};
    std::uint8_t keystream_[kKeystreamBlocks * kBlock] = {};
    std::size_t keystream_pos_ = sizeof keystream_;
};

// --- 7z AES -----------------------------------------------------------------------------------

struct SevenZipAesParams {
    unsigned cycles_power = 0;
    std::vector<std::uint8_t> salt;
    std::uint8_t iv[kBlock] = {};
};

SevenZipAesParams parse_7z_properties(const std::vector<std::uint8_t>& props)
{
    SevenZipAesParams params;
    if (props.empty()) {
        throw Error(ErrorCode::Corrupt, "7z: missing AES properties");
    }
    const std::uint8_t b0 = props[0];
    params.cycles_power = b0 & 0x3F;
    if ((b0 & 0xC0) == 0) {
        return params;
    }
    if (props.size() < 2) {
        throw Error(ErrorCode::Corrupt, "7z: invalid AES properties");
    }
    const std::uint8_t b1 = props[1];
    const std::size_t salt_size = ((b0 >> 7) & 1u) + (b1 >> 4);
    const std::size_t iv_size = ((b0 >> 6) & 1u) + (b1 & 0x0Fu);
    if (props.size() != 2 + salt_size + iv_size) {
        throw Error(ErrorCode::Corrupt, "7z: invalid AES properties");
    }
    params.salt.assign(props.begin() + 2, props.begin() + 2 + static_cast<std::ptrdiff_t>(salt_size));
    std::memcpy(params.iv, props.data() + 2 + salt_size, iv_size);
    return params;
}

// AES-256-CBC decryption (7z and RAR). The data must be a whole number of 16-byte blocks.
class CbcStream final : public io::InputStream {
public:
    CbcStream(io::StreamPtr upstream, const Key256& key, const std::uint8_t* iv)
        : upstream_(std::move(upstream)), aes_(key.data(), key.size()), buffer_(kChunkBlocks * kBlock)
    {
        std::memcpy(previous_, iv, kBlock);
    }

    std::size_t read(std::uint8_t* data, std::size_t size) override
    {
        if (pos_ == end_) {
            if (!refill()) {
                return 0;
            }
        }
        const std::size_t n = std::min(size, end_ - pos_);
        std::memcpy(data, plain_.data() + pos_, n);
        pos_ += n;
        return n;
    }

private:
    // CBC: each plaintext block is the decrypted block XOR the previous ciphertext block.
    bool refill()
    {
        const std::size_t got = upstream_->read_up_to(buffer_.data(), buffer_.size());
        if (got == 0) {
            return false;
        }
        if (got % kBlock != 0) {
            throw Error(ErrorCode::Corrupt, "encrypted data is not a whole number of AES blocks");
        }
        plain_.resize(got);
        aes_.decrypt(buffer_.data(), plain_.data(), got / kBlock);
        for (std::size_t b = 0; b < got; b += kBlock) {
            for (std::size_t i = 0; i < kBlock; ++i) {
                plain_[b + i] ^= previous_[i];
            }
            std::memcpy(previous_, buffer_.data() + b, kBlock);
        }
        pos_ = 0;
        end_ = got;
        return true;
    }

    io::StreamPtr upstream_;
    crypto::Aes aes_;
    std::vector<std::uint8_t> buffer_;
    std::vector<std::uint8_t> plain_;
    std::uint8_t previous_[kBlock] = {};
    std::size_t pos_ = 0;
    std::size_t end_ = 0;
};

} // namespace

io::StreamPtr decrypt_zipcrypto(io::StreamPtr upstream, const std::string& password, std::uint8_t check)
{
    return std::make_unique<ZipCryptoStream>(std::move(upstream), password, check);
}

io::StreamPtr decrypt_winzip_aes(io::StreamPtr upstream, std::uint64_t packed_size, unsigned strength,
                                 const std::string& password)
{
    return std::make_unique<WinZipAesStream>(std::move(upstream), packed_size, strength, password);
}

Key256 derive_7z_key(const std::vector<std::uint8_t>& properties, const std::string& password)
{
    const SevenZipAesParams params = parse_7z_properties(properties);
    std::string utf16 = text::utf8_to_utf16le(password);
    Key256 key{};

    if (params.cycles_power == 0x3F) {
        // No hashing: salt and password copied as they are.
        std::size_t pos = 0;
        for (const std::uint8_t b : params.salt) {
            if (pos < key.size()) {
                key[pos++] = b;
            }
        }
        for (const char c : utf16) {
            if (pos < key.size()) {
                key[pos++] = static_cast<std::uint8_t>(c);
            }
        }
        crypto::wipe(utf16.data(), utf16.size());
        return key;
    }
    if (params.cycles_power > 24) {
        throw Error(ErrorCode::Unsupported, "7z: too many key derivation rounds");
    }

    // SHA-256 over 2^cycles_power rounds of (salt, password, 64-bit round counter), fed in large
    // batches to keep the number of hash calls low.
    const std::size_t round_size = params.salt.size() + utf16.size() + 8;
    const std::uint64_t rounds = std::uint64_t{1} << params.cycles_power;
    const std::size_t batch = std::max<std::size_t>(1, (1u << 16) / round_size);
    std::vector<std::uint8_t> buffer(batch * round_size);
    for (std::size_t r = 0; r < batch; ++r) {
        std::uint8_t* p = buffer.data() + r * round_size;
        std::copy(params.salt.begin(), params.salt.end(), p);
        std::memcpy(p + params.salt.size(), utf16.data(), utf16.size());
    }
    crypto::Sha256 sha;
    for (std::uint64_t round = 0; round < rounds;) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(batch, rounds - round));
        for (std::size_t r = 0; r < count; ++r) {
            std::uint8_t* counter = buffer.data() + r * round_size + round_size - 8;
            const std::uint64_t value = round + r;
            for (int i = 0; i < 8; ++i) {
                counter[i] = static_cast<std::uint8_t>(value >> (8 * i));
            }
        }
        sha.update(buffer.data(), count * round_size);
        round += count;
    }
    key = sha.finish();
    crypto::wipe(buffer.data(), buffer.size());
    crypto::wipe(utf16.data(), utf16.size());
    return key;
}

std::vector<std::uint8_t> sevenzip_key_identity(const std::vector<std::uint8_t>& properties)
{
    const SevenZipAesParams params = parse_7z_properties(properties);
    std::vector<std::uint8_t> identity{static_cast<std::uint8_t>(params.cycles_power)};
    identity.insert(identity.end(), params.salt.begin(), params.salt.end());
    return identity;
}

io::StreamPtr decrypt_7z_aes(io::StreamPtr upstream, const std::vector<std::uint8_t>& properties, const Key256& key)
{
    return std::make_unique<CbcStream>(std::move(upstream), key, parse_7z_properties(properties).iv);
}

Rar5Keys derive_rar5_keys(const std::string& password, const std::uint8_t* salt, unsigned log2_count)
{
    if (log2_count > 24) {
        throw Error(ErrorCode::Unsupported, "rar: too many key derivation rounds");
    }
    // PBKDF2-HMAC-SHA256 with 2^n rounds gives the key; 16 and 32 more rounds give the hash key
    // and the password check value.
    const auto* pw = reinterpret_cast<const std::uint8_t*>(password.data());
    const unsigned count = 1u << log2_count;
    Rar5Keys keys;
    crypto::pbkdf2(crypto::Hash::Sha256, pw, password.size(), salt, 16, count, keys.key.data(), keys.key.size());
    crypto::pbkdf2(crypto::Hash::Sha256, pw, password.size(), salt, 16, count + 16, keys.hash_key.data(),
                   keys.hash_key.size());
    Key256 check{};
    crypto::pbkdf2(crypto::Hash::Sha256, pw, password.size(), salt, 16, count + 32, check.data(), check.size());
    for (std::size_t i = 0; i < check.size(); ++i) {
        keys.check[i % keys.check.size()] ^= check[i];
    }
    crypto::wipe(check.data(), check.size());
    return keys;
}

io::StreamPtr decrypt_aes256_cbc(io::StreamPtr upstream, const Key256& key, const std::uint8_t* iv)
{
    return std::make_unique<CbcStream>(std::move(upstream), key, iv);
}

void decrypt_aes256_cbc(const Key256& key, const std::uint8_t* iv, std::uint8_t* data, std::size_t size)
{
    if (size % kBlock != 0) {
        throw Error(ErrorCode::Corrupt, "encrypted data is not a whole number of AES blocks");
    }
    crypto::Aes aes(key.data(), key.size());
    std::vector<std::uint8_t> cipher(data, data + size);
    aes.decrypt(cipher.data(), data, size / kBlock);
    for (std::size_t b = 0; b < size; b += kBlock) {
        const std::uint8_t* previous = b == 0 ? iv : cipher.data() + b - kBlock;
        for (std::size_t i = 0; i < kBlock; ++i) {
            data[b + i] ^= previous[i];
        }
    }
}

} // namespace digzip::codec
