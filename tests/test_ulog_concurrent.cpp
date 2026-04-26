#include "ulog.h"
#include <emmintrin.h>
#include <vector>
#include <thread>

#define call(a, b) 0

void busy_wait_cycles() 
{
    for (int i = 0; i < 500; i++)
    {
        _mm_pause();
    }
}
int main(int argc, const char* argv[])
{
    uco::OpenLog("test", LogLevel::DEBUG, LogMode::FILE, true);
    std::vector<std::thread> threads;
    auto now = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10; i++)
    {
      threads.emplace_back([] {
          int argc = 0;
          for (int i = 0; i < 10000; i++)
          {
              LOGERR("Error, ret is:", NR(argc), ',', call(2, 3), !true, 1 != argc, "!");
              ++argc;
              busy_wait_cycles();
          }
      });
    }
    for (auto&& t : threads)
    {
        t.join();
    }
    auto now1 = std::chrono::high_resolution_clock::now();
    auto dur = std::chrono::duration_cast<std::chrono::milliseconds>(now1 - now).count();
    printf("%zdms\n", dur);
    return 0;
}
