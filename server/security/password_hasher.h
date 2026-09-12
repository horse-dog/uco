#pragma once

#include "core/thread_pool.h"
#include "core/uco.h"

namespace webserver
{
namespace security
{

class IPasswordHasher
{
  public:
    virtual ~IPasswordHasher() = default;

    // 0: success, -1: error.
    virtual uco::task<int> Hash(const std::string &plainpassword,
                                std::string &hash) = 0;

    // 0: same, 1: not same, -1: error.
    virtual uco::task<int> Verify(const std::string &plainpassword,
                                  const std::string &hash) = 0;

    // 用户不存在时执行等成本校验: 0 完成, -1 错误.
    virtual uco::task<int> VerifyDummy(const std::string &plainpassword) = 0;
};

class BcryptPasswordHasher final : public IPasswordHasher
{
  public:
    explicit BcryptPasswordHasher(uco::thread_pool &cpu_pool)
        : m_cpu_pool(cpu_pool)
    {
    }

    uco::task<int> Hash(const std::string &plainpassword,
                        std::string &hash) override;

    // 0: same, 1: not same, -1: error.
    uco::task<int> Verify(const std::string &plainpassword,
                          const std::string &hash) override;

    uco::task<int> VerifyDummy(const std::string &plainpassword) override;

  private:
    uco::thread_pool &m_cpu_pool;
};

} // namespace security
} // namespace webserver
