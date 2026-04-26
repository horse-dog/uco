#pragma once
#include <string>
#include <string_view>
#include <google/protobuf/message.h>

bool MessageToJson(const google::protobuf::Message &message, std::string& json);

bool JsonToMessage(const std::string& json, google::protobuf::Message &message);

bool JsonToMessage(std::string_view json, google::protobuf::Message &message);
