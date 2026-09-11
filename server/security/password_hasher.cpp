#include "server/security/password_hasher.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <crypt.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace webserver
{
namespace security
{

static const std::string kDummyPasswordHash =
    "$2b$12$Eg5RBvY5HDChxhRuft8Y1OYYgutczUPpsEk.VeWfD2X80TJe.qdeC";

/// bcrypt 哈希 (cost 12, 随机盐); 失败时清空 hash.
static void bcrypt_hash(const std::string &plain, std::string &hash)
{
    hash.clear();
    std::array<unsigned char, 16> rnd{};
    if (RAND_bytes(rnd.data(), static_cast<int>(rnd.size())) != 1)
    {
        LOGERR("bcrypt: rand_bytes failed for salt");
        return;
    }

    std::array<char, CRYPT_GENSALT_OUTPUT_SIZE> setting{};
    if (crypt_gensalt_rn("$2b$", 12,
                         reinterpret_cast<const char *>(rnd.data()),
                         static_cast<int>(rnd.size()), setting.data(),
                         static_cast<int>(setting.size())) == nullptr)
    {
        LOGERR("bcrypt: crypt_gensalt_rn failed, errno:", errno,
               "msg:", strerror(errno));
        return;
    }

    struct crypt_data data {};
    char *out = crypt_r(plain.c_str(), setting.data(), &data);
    if (out == nullptr)
    {
        LOGERR("bcrypt: crypt_r failed");
        return;
    }
    hash.assign(out);
}

/// bcrypt 校验: 恒时比对 (CRYPTO_memcmp 防时序侧信道).
static bool bcrypt_verify(const std::string &plain, const std::string &hash)
{
    if (hash.rfind("$2b$", 0) != 0 || hash.size() != 60)
    {
        return false; // 存量数据异常, 拒绝.
    }
    struct crypt_data data
    {
    };
    char *out = crypt_r(plain.c_str(), hash.c_str(), &data);
    if (out == nullptr)
    {
        return false;
    }
    // std::this_thread::sleep_for(std::chrono::seconds(8));
    return CRYPTO_memcmp(out, hash.data(), hash.size()) == 0;
}

uco::task<int> BcryptPasswordHasher::Hash(const std::string &plainpassword,
                                          std::string &hash)
{
    int ret = co_await m_cpu_pool.execute(bcrypt_hash, plainpassword, hash);
    if (ret != 0 || hash.empty())
    {
        LOGERR("hash failed, ret:", ret);
        co_return -1;
    }
    co_return 0;
}

uco::task<int> BcryptPasswordHasher::Verify(const std::string &plainpassword,
                                             const std::string &hash)
{
    bool same = false;
    int ret = 0;
    ret = co_await m_cpu_pool.execute(
        [&] { same = bcrypt_verify(plainpassword, hash); });
    if (ret != 0)
    {
        LOGERR("verify failed, ret:", ret);
        co_return -1;
    }
    co_return same ? 0 : 1;
}

uco::task<int>
BcryptPasswordHasher::VerifyDummy(const std::string &plainpassword)
{
    int ret = co_await Verify(plainpassword, kDummyPasswordHash);
    co_return ret < 0 ? -1 : 0;
}

} // namespace security
} // namespace webserver
