#include "json.h"
#include "ulog.h"

#include <google/protobuf/util/json_util.h>
#include <string_view>

bool MessageToJson(const google::protobuf::Message &message, std::string &json)
{
    using namespace google::protobuf::util;
    // 保留 proto 原字段名 (snake_case 直出), 与 proto 定义一致,
    // 免前端按字段名直觉取值时踩 camelCase 转换的坑;
    // 解析端 JsonStringToMessage 本就同时接受两种命名, 不受影响.
    JsonPrintOptions opts;
    opts.preserve_proto_field_names = true;
    auto ret = MessageToJsonString(message, &json, opts);
    if (!ret.ok())
    {
        LOGERR("message -> json failed, reason: %s, message: %s",
               ret.message().data(), message.ShortDebugString().data());
    }
    return ret.ok();
}

bool JsonToMessage(const std::string &json, google::protobuf::Message &message)
{
    using namespace google::protobuf::util;
    auto ret = JsonStringToMessage(json, &message);
    if (!ret.ok())
    {
        LOGERR("json -> message failed, reason: %s, json: %s",
               ret.message().data(), json.data());
    }
    return ret.ok();
}

bool JsonToMessage(std::string_view json, google::protobuf::Message &message)
{
    using namespace google::protobuf::util;
    auto ret = JsonStringToMessage(json, &message);
    if (!ret.ok())
    {
        LOGERR("json -> message failed, reason: %s, json: %s",
               ret.message().data(), json.data());
    }
    return ret.ok();
}
