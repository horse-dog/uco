#pragma once

#include <cstdint>
#include <string>

#define LOG_NODE_IDX_NULL UINT64_MAX
#define LOG_NODE_SIZE     2048
#define LOG_NODE_DATA_MAX_SIZE (LOG_NODE_SIZE - 24)
#define SHM_LOG_DATA_SIZE (256 * 1024 * 1024) // 256mb.
#define LOG_NODE_COUNT    (SHM_LOG_DATA_SIZE / LOG_NODE_SIZE)
#define LOG_SHM_MAGIC     0x4C4F4753ULL // "LOGS"

typedef struct LogNode
{
    uint64_t next;
    uint64_t log_len;
    uint64_t timestamp;
    char log_data[0];
} LogNode_t;

typedef struct LockFreeStack
{
    uint64_t top;
} LockFreeStack_t;

int      ulog_init(bool create = false);
void     ulog_push(uint64_t timestamp, std::string& log);
uint64_t log_shm_drain_fg_stack();
char*    log_shm_get_node_array();
