#include "ulog.h"
#include <vector>

#define call(a, b) 0

int main(int argc, const char* argv[])
{
    uco::OpenLog("test", LogLevel::DEBUG, LogMode::FILE, true);
    LOGERR("Test", "hello, world", ',', +1e6, .12,   call(2, 3), ",", call("hello", "hello, world"), argc, NR(argc), ".");

    SYSDBG("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    SYSMSG("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    SYSWRN("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    SYSERR("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");

    LOGDBG("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    LOGMSG("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    LOGWRN("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    LOGERR("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");

    std::vector<std::string> vec {"hello", "world"};
    std::optional<std::string> p;
    std::pair<int, int> p1 {1, 2};
    std::map<int, int> m {{1, 2}, {3, 4}};
    LOGERR(vec, p, p1, "m is:", NR(m));
    LOGFTL("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
    return 0;
}
