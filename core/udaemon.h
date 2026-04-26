#pragma once
#include <string>

namespace uco
{

void InitProcess(bool bDaemonize, const std::string &lock_name = "");

} // namespace uco
