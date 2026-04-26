#include "ulogshm_def.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


static inline uint64_t atomic_read_uint64(uint64_t *ptr)
{
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void atomic_write_uint64(uint64_t *ptr, uint64_t val)
{
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

static inline bool atomic_cas_uint64(uint64_t *ptr, uint64_t expected, uint64_t desired)
{
    return __atomic_compare_exchange_n(ptr, &expected, desired, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline void lockfree_stack_init(LockFreeStack_t *stack)
{
    atomic_write_uint64(&stack->top, LOG_NODE_IDX_NULL);
}

static inline void lockfree_stack_push(LockFreeStack_t *stack, char *node_base, uint64_t node_idx)
{
    uint64_t old_top;
    LogNode_t *node = (LogNode_t *)(node_base + node_idx * LOG_NODE_SIZE);
    do
    {
        old_top = atomic_read_uint64(&stack->top);
        node->next = old_top;
    } while (!atomic_cas_uint64(&stack->top, old_top, node_idx));
}

static inline uint64_t lockfree_stack_xchg_to_null(LockFreeStack_t *stack)
{
    return __atomic_exchange_n(&stack->top, LOG_NODE_IDX_NULL, __ATOMIC_ACQ_REL);
}


typedef struct LogShmHeader
{
    uint64_t magic;
    uint64_t current_index;
    LockFreeStack_t stack_fg;
    char padding[LOG_NODE_SIZE - sizeof(magic) - sizeof(current_index) - sizeof(stack_fg)];
} LogShmHeader_t;

#define LOG_SHM_HEADER_SIZE sizeof(LogShmHeader_t) // 应为 128
#define SHM_TOTAL_SIZE (LOG_SHM_HEADER_SIZE + SHM_LOG_DATA_SIZE)
#define SHM_LOG_NAME       "/tmp/uco_log_shm"

LogShmHeader_t* g_log_shm = nullptr;

static inline uint64_t atomic_fetch_add_uint64(uint64_t *ptr, uint64_t val)
{
    return __atomic_fetch_add(ptr, val, __ATOMIC_ACQ_REL);
}

char *log_shm_get_node_array()
{
    return (char *)g_log_shm + LOG_SHM_HEADER_SIZE;
}

static inline LogNode_t *log_shm_get_node(uint64_t index)
{
    uint64_t slot = index % LOG_NODE_COUNT;
    char *base = log_shm_get_node_array();
    return (LogNode_t *)(base + slot * LOG_NODE_SIZE);
}

static inline uint64_t log_shm_node_to_index(LogNode_t *node)
{
    char *base = log_shm_get_node_array();
    char *node_char = (char *)node;
    return (uint64_t)(node_char - base) / LOG_NODE_SIZE;
}

static inline LogNode_t *log_shm_alloc_node()
{
    uint64_t idx = atomic_fetch_add_uint64(&g_log_shm->current_index, 1);
    auto node = log_shm_get_node(idx);
    node->next = LOG_NODE_IDX_NULL;
    return node;
}

static inline void log_shm_write_node(LogNode_t *node, const char *log_data, uint64_t log_len)
{
    log_len = std::min(log_len, (uint64_t)LOG_NODE_DATA_MAX_SIZE);
    node->log_len = log_len;
    memcpy(node->log_data, log_data, log_len);
}

static inline void log_shm_push_node(LogNode_t *node)
{
    uint64_t idx = log_shm_node_to_index(node);
    lockfree_stack_push(&g_log_shm->stack_fg, log_shm_get_node_array(), idx);
}

uint64_t log_shm_drain_fg_stack()
{
    return lockfree_stack_xchg_to_null(&(g_log_shm->stack_fg));
}

int ulog_init(bool create)
{
    bool need_init = false;
    int fd = open(SHM_LOG_NAME, O_RDWR, 0666);

    if (fd == -1)
    {
        if (!create)
        {
            printf("Shared memory %s not exist. Start log consumer first!\n", SHM_LOG_NAME);
            return -1;
        }

        fd = open(SHM_LOG_NAME, O_CREAT | O_RDWR, 0666);
        if (fd == -1)
        {
            perror("create shm failed");
            return -1;
        }

        if (ftruncate(fd, SHM_TOTAL_SIZE) == -1)
        {
            perror("ftruncate failed");
            close(fd);
            return -1;
        }

        need_init = true;
    }

    void *ptr = mmap(NULL, SHM_TOTAL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED)
    {
        perror("mmap failed");
        close(fd);
        return -1;
    }

    close(fd);

    LogShmHeader_t *shm = (LogShmHeader_t *)ptr;

    if (need_init)
    {
        memset(shm, 0, SHM_TOTAL_SIZE);

        // init header
        atomic_write_uint64(&shm->magic, LOG_SHM_MAGIC);
        atomic_write_uint64(&shm->current_index, 0);
        lockfree_stack_init(&shm->stack_fg);
    }
    else
    {
        uint64_t magic = atomic_read_uint64(&shm->magic);
        if (magic != LOG_SHM_MAGIC)
        {
            printf("Magic verification failed.\n");
            munmap(shm, SHM_TOTAL_SIZE);
            return -1;
        }
    }

    g_log_shm = shm;
    return 0;
}

void ulog_push(uint64_t timestamp, std::string& log)
{
    if (g_log_shm == nullptr || log.empty())
    {
        printf("g_log_shm\n");
        return;
    }

    log.push_back('\n');
    uint64_t log_len = std::min(log.size(), (uint64_t)LOG_NODE_DATA_MAX_SIZE);
    LogNode_t* node = log_shm_alloc_node();
    node->timestamp = timestamp;
    log_shm_write_node(node, log.c_str(), log_len);
    log_shm_push_node(node);
}
