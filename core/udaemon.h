#pragma once
#include <string>
#include <vector>

namespace uco
{

void InitProcess(bool bDaemonize, const std::string &lock_name = "",
                 const std::vector<int> &blocked_signals = {});

} // namespace uco
