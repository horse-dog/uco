#include "json.h"
#include "ulog.h"

#include <google/protobuf/util/json_util.h>
#include <string_view>

bool MessageToJson(const google::protobuf::Message &message, std::string &json)
{
    using namespace google::protobuf::util;
    auto ret = MessageToJsonString(message, &json);
    if (!ret.ok())
    {
        SYSERR("message -> json failed, reason: %s, message: %s",
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
        SYSERR("json -> message failed, reason: %s, json: %s",
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
        SYSERR("json -> message failed, reason: %s, json: %s",
               ret.message().data(), json.data());
    }
    return ret.ok();
}
