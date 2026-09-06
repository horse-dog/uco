#pragma once

// 调试宏开关
#define USE_FRAMEWORK_DBG 0
#define USE_MYSQL_DEBUG 0
#define USE_REDIS_DEBUG 0

// 选项
#define UCO_TOLERATE_ROOT_EXCEPTIONS 0 // // 协程抛出异常，进程是否终止: 0: terminate, 1: tolerate.
