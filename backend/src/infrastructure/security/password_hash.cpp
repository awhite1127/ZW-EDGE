// 密码哈希实现：使用随机 salt 和 PBKDF2-HMAC-SHA256，并采用常量时间比较校验摘要。
#include "infrastructure/security/password_hash.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace edge_controller::security {

namespace {

constexpr std::size_t kSha256BlockSize = 64;
constexpr std::size_t kSha256DigestSize = 32;
constexpr std::size_t kPasswordSaltBytes = 16;
constexpr std::uint32_t kMaxPasswordHashIterations = 1000000;

using Sha256Digest = std::array<std::uint8_t, kSha256DigestSize>;
using PasswordSalt = std::array<std::uint8_t, kPasswordSaltBytes>;

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

// 对 32 位字执行循环右移。
std::uint32_t rotate_right(std::uint32_t value, std::uint32_t bits)
{
    return (value >> bits) | (value << (32U - bits));
}

// 按大端字节序读取 32 位整数。
std::uint32_t load_be32(const std::uint8_t* data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

// 按大端字节序写入 32 位整数。
void store_be32(std::uint32_t value, std::uint8_t* output)
{
    output[0] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
    output[1] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
    output[2] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
    output[3] = static_cast<std::uint8_t>(value & 0xFFU);
}

struct Sha256Context {
    std::array<std::uint32_t, 8> state = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
    };
    std::array<std::uint8_t, kSha256BlockSize> buffer{};
    std::size_t buffer_size{0};
    std::uint64_t total_bytes{0};
};

// 执行一轮 SHA-256 压缩变换。
void sha256_transform(Sha256Context* context, const std::uint8_t* block)
{
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        words[i] = load_be32(block + i * 4U);
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto s0 = rotate_right(words[i - 15], 7) ^ rotate_right(words[i - 15], 18) ^ (words[i - 15] >> 3U);
        const auto s1 = rotate_right(words[i - 2], 17) ^ rotate_right(words[i - 2], 19) ^ (words[i - 2] >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    auto a = context->state[0];
    auto b = context->state[1];
    auto c = context->state[2];
    auto d = context->state[3];
    auto e = context->state[4];
    auto f = context->state[5];
    auto g = context->state[6];
    auto h = context->state[7];

    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const auto ch = (e & f) ^ ((~e) & g);
        const auto temp1 = h + s1 + ch + kSha256RoundConstants[i] + words[i];
        const auto s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

// 向 SHA-256 上下文追加输入数据。
void sha256_update(Sha256Context* context, const std::uint8_t* data, std::size_t length)
{
    context->total_bytes += length;
    if (context->buffer_size != 0U) {
        const auto copy_size = std::min(length, kSha256BlockSize - context->buffer_size);
        std::copy_n(data, copy_size, context->buffer.data() + context->buffer_size);
        context->buffer_size += copy_size;
        data += copy_size;
        length -= copy_size;
        if (context->buffer_size == kSha256BlockSize) {
            sha256_transform(context, context->buffer.data());
            context->buffer_size = 0;
        }
    }
    while (length >= kSha256BlockSize) {
        sha256_transform(context, data);
        data += kSha256BlockSize;
        length -= kSha256BlockSize;
    }
    if (length != 0U) {
        std::copy_n(data, length, context->buffer.data());
        context->buffer_size = length;
    }
}

// 完成 SHA-256 计算并输出摘要。
Sha256Digest sha256_final(Sha256Context context)
{
    const auto bit_length = context.total_bytes * 8ULL;
    context.buffer[context.buffer_size++] = 0x80U;
    if (context.buffer_size > 56U) {
        std::fill(context.buffer.begin() + static_cast<std::ptrdiff_t>(context.buffer_size), context.buffer.end(), 0U);
        sha256_transform(&context, context.buffer.data());
        context.buffer_size = 0;
    }
    std::fill(
        context.buffer.begin() + static_cast<std::ptrdiff_t>(context.buffer_size),
        context.buffer.begin() + 56,
        0U);
    for (std::size_t i = 0; i < 8; ++i) {
        context.buffer[56U + i] = static_cast<std::uint8_t>((bit_length >> (56U - i * 8U)) & 0xFFU);
    }
    sha256_transform(&context, context.buffer.data());

    Sha256Digest digest{};
    for (std::size_t i = 0; i < context.state.size(); ++i) {
        store_be32(context.state[i], digest.data() + i * 4U);
    }
    return digest;
}

// 计算输入数据的 SHA-256 摘要。
Sha256Digest sha256(const std::uint8_t* data, std::size_t length)
{
    Sha256Context context;
    sha256_update(&context, data, length);
    return sha256_final(context);
}

class HmacSha256 {
public:

    explicit HmacSha256(const std::string& key)
    {
        std::array<std::uint8_t, kSha256BlockSize> normalized_key{};
        if (key.size() > normalized_key.size()) {
            const auto digest = sha256(reinterpret_cast<const std::uint8_t*>(key.data()), key.size());
            std::copy(digest.begin(), digest.end(), normalized_key.begin());
        } else {
            std::copy(key.begin(), key.end(), normalized_key.begin());
        }

        std::array<std::uint8_t, kSha256BlockSize> inner_pad{};
        std::array<std::uint8_t, kSha256BlockSize> outer_pad{};
        for (std::size_t i = 0; i < normalized_key.size(); ++i) {
            inner_pad[i] = static_cast<std::uint8_t>(normalized_key[i] ^ 0x36U);
            outer_pad[i] = static_cast<std::uint8_t>(normalized_key[i] ^ 0x5cU);
        }
        sha256_update(&inner_seed_, inner_pad.data(), inner_pad.size());
        sha256_update(&outer_seed_, outer_pad.data(), outer_pad.size());
    }

    // 计算当前 HMAC-SHA256 输入的摘要。
    Sha256Digest digest(const std::uint8_t* data, std::size_t length) const
    {
        auto inner = inner_seed_;
        sha256_update(&inner, data, length);
        const auto inner_digest = sha256_final(inner);

        auto outer = outer_seed_;
        sha256_update(&outer, inner_digest.data(), inner_digest.size());
        return sha256_final(outer);
    }

private:
    Sha256Context inner_seed_;
    Sha256Context outer_seed_;
};

// 使用 PBKDF2-HMAC-SHA256 派生密码摘要。
Sha256Digest pbkdf2_hmac_sha256(
    const std::string& password,
    const PasswordSalt& salt,
    std::uint32_t iterations)
{
    HmacSha256 hmac(password);
    std::array<std::uint8_t, kPasswordSaltBytes + 4> block_input{};
    std::copy(salt.begin(), salt.end(), block_input.begin());
    block_input.back() = 1U;

    auto u = hmac.digest(block_input.data(), block_input.size());
    auto result = u;
    for (std::uint32_t i = 1; i < iterations; ++i) {
        u = hmac.digest(u.data(), u.size());
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = static_cast<std::uint8_t>(result[index] ^ u[index]);
        }
    }
    return result;
}

// 将字节数组编码为十六进制文本。
template <std::size_t Size>
// 将字节数组编码为十六进制文本。
std::string to_hex(const std::array<std::uint8_t, Size>& bytes)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return stream.str();
}

// 将单个十六进制字符转换为数值。
int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

// 将十六进制文本解码为字节数组。
template <std::size_t Size>
// 将十六进制文本解码为字节数组。
bool from_hex(const std::string& value, std::array<std::uint8_t, Size>* bytes)
{
    if (bytes == nullptr || value.size() != Size * 2U) {
        return false;
    }
    for (std::size_t i = 0; i < Size; ++i) {
        const auto high = hex_value(value[i * 2U]);
        const auto low = hex_value(value[i * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        (*bytes)[i] = static_cast<std::uint8_t>((high << 4U) | low);
    }
    return true;
}

// 生成密码盐使用的安全随机字节。
bool random_bytes(PasswordSalt* bytes)
{
    if (bytes == nullptr) {
        return false;
    }
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (random) {
        random.read(reinterpret_cast<char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        if (random.gcount() == static_cast<std::streamsize>(bytes->size())) {
            return true;
        }
    }

    try {
        std::random_device device;
        for (auto& byte : *bytes) {
            byte = static_cast<std::uint8_t>(device() & 0xFFU);
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

// 以常量时间比较两个摘要。
bool constant_time_equal(const Sha256Digest& left, const Sha256Digest& right)
{
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        diff = static_cast<std::uint8_t>(diff | (left[i] ^ right[i]));
    }
    return diff == 0;
}

}  // namespace

// 创建密码哈希。
bool create_password_hash(
    const std::string& password,
    std::uint32_t iterations,
    std::string* hash_hex,
    std::string* salt_hex,
    std::string* error_message)
{
    if (hash_hex == nullptr || salt_hex == nullptr) {
        if (error_message != nullptr) {
            *error_message = "密码哈希输出参数为空";
        }
        return false;
    }
    if (iterations == 0 || iterations > kMaxPasswordHashIterations) {
        if (error_message != nullptr) {
            *error_message = "密码哈希迭代次数非法";
        }
        return false;
    }
    PasswordSalt salt{};
    if (!random_bytes(&salt)) {
        if (error_message != nullptr) {
            *error_message = "生成密码 salt 失败";
        }
        return false;
    }
    const auto hash = pbkdf2_hmac_sha256(password, salt, iterations);
    *salt_hex = to_hex(salt);
    *hash_hex = to_hex(hash);
    return true;
}

// 验证密码。
bool verify_password(
    const std::string& password,
    const std::string& hash_hex,
    const std::string& salt_hex,
    std::uint32_t iterations)
{
    if (iterations == 0 || iterations > kMaxPasswordHashIterations) {
        return false;
    }
    Sha256Digest expected{};
    PasswordSalt salt{};
    if (!from_hex(hash_hex, &expected) || !from_hex(salt_hex, &salt)) {
        return false;
    }
    const auto actual = pbkdf2_hmac_sha256(password, salt, iterations);
    return constant_time_equal(actual, expected);
}

}  // namespace edge_controller::security
