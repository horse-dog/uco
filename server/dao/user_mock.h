#pragma once

/**
 * @file user_mock.h
 * @brief user 表 dao 的 mock 实现 (单测用, header-only).
 *
 * 用法: 预设各接口返回码 (ret_xxx) 与 Rsp 填充器 (fill_xxx),
 *       注入 service 后断言行为; xxx_reqs 记录收到的请求供校验入参.
 *
 * @code
 *   MockUserDao dao;
 *   dao.ret_insert_user = 1; // 模拟用户名已占用
 *   UserService svc(dao);
 *   ... 断言 svc.Register(...) 返回"用户名已占用";
 *   assert(dao.insert_reqs.size() == 1);
 * @endcode
 */

#include "server/dao/user.h"

#include <functional>
#include <vector>

namespace webserver
{
namespace dao
{

class MockUserDao final : public IUserDao
{
  public:
    // ---- 预设 (默认全 0 = 成功) ----

    int ret_insert_user = 0;
    int ret_query_by_username = 0;
    int ret_query_by_vid = 0;
    int ret_update_password = 0;

    std::function<void(dao::InsertUserRsp &)> fill_insert_user; ///< Rsp 填充器 (空则不填充).
    std::function<void(dao::QueryByUsernameRsp &)> fill_query_by_username;
    std::function<void(dao::QueryByVidRsp &)> fill_query_by_vid;
    std::function<void(dao::UpdatePasswordRsp &)> fill_update_password;

    // ---- 调用记录 (断言入参/次数用) ----

    std::vector<dao::InsertUserReq> insert_reqs;
    std::vector<dao::QueryByUsernameReq> query_by_username_reqs;
    std::vector<dao::QueryByVidReq> query_by_vid_reqs;
    std::vector<dao::UpdatePasswordReq> update_password_reqs;

    uco::task<int> InsertUser(const dao::InsertUserReq &req,
                              dao::InsertUserRsp &rsp) override
    {
        insert_reqs.push_back(req);
        if (fill_insert_user)
        {
            fill_insert_user(rsp);
        }
        co_return ret_insert_user;
    }

    uco::task<int> QueryByUsername(const dao::QueryByUsernameReq &req,
                                   dao::QueryByUsernameRsp &rsp) override
    {
        query_by_username_reqs.push_back(req);
        if (fill_query_by_username)
        {
            fill_query_by_username(rsp);
        }
        co_return ret_query_by_username;
    }

    uco::task<int> QueryByVid(const dao::QueryByVidReq &req,
                              dao::QueryByVidRsp &rsp) override
    {
        query_by_vid_reqs.push_back(req);
        if (fill_query_by_vid)
        {
            fill_query_by_vid(rsp);
        }
        co_return ret_query_by_vid;
    }

    uco::task<int> UpdatePassword(const dao::UpdatePasswordReq &req,
                                  dao::UpdatePasswordRsp &rsp) override
    {
        update_password_reqs.push_back(req);
        if (fill_update_password)
        {
            fill_update_password(rsp);
        }
        co_return ret_update_password;
    }
};

} // namespace dao
} // namespace webserver
