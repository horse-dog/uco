// env_hook.cc - LD_PRELOAD Hook 库：确保 env 最先析构
//
// 策略: Inline Hook __call_tls_dtors（glibc TLS 析构入口）
//
// 为什么需要 inline hook?
//   __call_tls_dtors 是 glibc 执行所有 thread_local dtor 的入口，
//   但它在 libc 内部通过直接 call 调用（不经过 PLT），
//   所以 LD_PRELOAD 符号拦截无效。
//   解决办法：运行时直接改写 __call_tls_dtors 开头的机器码，
//   使其跳转到我们的 wrapper 函数。
//
// Inline Hook 原理 (x86_64):
//   1. dlsym 拿到真实 __call_tls_dtors 地址
//   2. 保存原始前 N 字节指令到可执行 trampoline
//   3. trampoline 末尾跳回原始地址 + N 处继续执行
//   4. 用 mprotect 将原始页面改为可写
//   5. 覆盖前 N 字节为 jmp 到我们的 hook 函数
//   6. hook 函数先执行 env_dtor()，再调用 trampoline（即原始逻辑）
//
// 编译: g++ -O2 -std=c++20 -fPIC -shared -o libenvhook.so env_hook.cc
// 运行: LD_PRELOAD=./libenvhook.so ./demo

#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <sys/mman.h>

// ====== 公开导出的共享状态 ======

// __attribute__((visibility("default")))
// __thread bool shared_env_dead = false;

// ====== 内部状态 ======

static void* real_cxa_atexit = NULL;
static void* real_call_tls_dtors_addr = NULL;  // 原始 __call_tls_dtors 地址
static void* g_trampoline = nullptr;            // 可执行的 trampoline 缓冲区

// __call_tls_dtors 的前几条指令（glibc 2.x，x86_64）：
//   endbr64                        ; f3 0f 1e fa   (4 bytes)
//   push   %rbp                    ; 55            (1 byte)
//   mov    %rsp,%rbp               ; 48 89 e5      (3 bytes)
// 前 8 字节覆盖两条完整指令
// 使用 FF 25 00 00 00 00 (jmp [rip]+absaddr) 消除 ±2GB 限制：
//   6 字节指令 + 8 字节地址 = 14 字节/个网关
static constexpr size_t HOOK_SIZE = 8;
static constexpr size_t ABS_JMP_GATEWAY_SIZE = 14;  // FF 25 00 00 00 00 + int64 addr
static uint8_t g_orig_bytes[HOOK_SIZE];
static __thread void(*__hook_before_tls_dtor)(void) = 0;

// ====== Trampoline：执行原始前 HOOK_SIZE 字节的指令，然后跳回继续 ======
// 由编译器生成；在运行时动态填充机器码

// ====== 我们的 Wrapper：在转发前执行 env 清理 ======

// 使用 naked 函数或汇编来精确控制 trampoline
// 这里用更安全的方式：让 wrapper 直接调用原始 trampoline

extern "C" {

// ====== 我们的 Wrapper：在转发前执行 env 清理 ======
//
// 调用流程说明：
//   原始 call site:  call __call_tls_dtors   ; push ret_addr, jmp to func
//   被 hook 后:      jmp tls_dtors_hook       ; 仅 jmp（不 push，栈顶仍是调用方 ret_addr）
//   栈状态:          [原始调用方的返回地址]
//   tls_dtors_hook 作为普通 C 函数执行：
//     prologue: push %rbp; mov %rsp,%rbp       ; 正常保存帧指针
//     ... 执行 env_dtor + 调用 trampoline ...
//     epilogue: pop %rbp; ret                  ; ret 弹出调用方返回地址，正确返回 ★
//
// 关键洞察：jmp + 普通C函数的 ret = 完美的调用约定匹配！

static void (*g_trampoline_fn)(void) = nullptr;

static void tls_dtors_hook(void) {
    // ★ 防止重复执行 dtor 链（__call_tls_dtors 有多个 call site）
    static __thread bool called = false;
    if (!called) {
        called = true;
        // ★ 先执行注册的回调
        if (__hook_before_tls_dtor)
        {
          __hook_before_tls_dtor();
        }
        // 第一次：通过 C 函数调用执行完整的原始逻辑
        if (g_trampoline_fn) g_trampoline_fn();
    }
    // 第二次及之后：直接返回（dtor 链已空），ret 会回到原始调用方
}

} // extern "C"

// ====== 安装 inline hook ======

static bool install_inline_hook(void) {
    // Step 1: 获取真实 __call_tls_dtors 地址
    real_call_tls_dtors_addr = dlsym(RTLD_DEFAULT, "__call_tls_dtors");
    if (!real_call_tls_dtors_addr) {
        const char err[] = "[FATAL] dlsym(__call_tls_dtors) failed\n";
        int ret = write(STDERR_FILENO, err, sizeof(err) - 1);
        return false;
    }

    // Step 2: 分配可执行的 trampoline 内存
    // 布局:
    //   [0..HOOK_SIZE)              : 原始指令副本
    //   [HOOK_SIZE..+ABS_JMP_GATEWAY_SIZE) : 网关: jmp abs → 原始函数 + HOOK_SIZE（trampoline回跳）
    //   [+ABS_JMP_GATEWAY_SIZE, +ABS_JMP_GATEWAY_SIZE*2) : 网关: jmp abs → hook 函数（hook回跳网关）
    static constexpr size_t TRAMPOLINE_SIZE = HOOK_SIZE + ABS_JMP_GATEWAY_SIZE * 2;
    g_trampoline = mmap(NULL, TRAMPOLINE_SIZE,
                        PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_trampoline == MAP_FAILED) {
        const char err[] = "[FATAL] mmap trampoline failed\n";
        int ret = write(STDERR_FILENO, err, sizeof(err) - 1);
        return false;
    }

    // Step 3: 保存原始字节并构建 trampoline
    memcpy(g_orig_bytes, real_call_tls_dtors_addr, HOOK_SIZE);

    uint8_t* tramp = (uint8_t*)g_trampoline;

    // [0] 原始指令副本
    memcpy(tramp, g_orig_bytes, HOOK_SIZE);

    // [HOOK_SIZE] 网关1: jmp abs → __call_tls_dtors + HOOK_SIZE（trampoline 执行完后跳回原始函数）
    //   FF 25 00 00 00 00        ; jmp [rip]  (rip 指向下一条指令，即后面的地址)
    //   <8-byte absolute address> ; 目标地址
    {
        uintptr_t target = (uintptr_t)real_call_tls_dtors_addr + HOOK_SIZE;
        uint8_t* gw = tramp + HOOK_SIZE;
        gw[0] = 0xFF; gw[1] = 0x25; gw[2] = gw[3] = gw[4] = gw[5] = 0x00;  // jmp [rip]
        memcpy(gw + 6, &target, sizeof(target));
    }

    // [HOOK_SIZE + ABS_JMP_GATEWAY_SIZE] 网关2: jmp abs → tls_dtors_hook（patch 跳转目标）
    //   同样用 FF 25 绝对跳转，消除 ±2GB 限制
    uintptr_t hook_addr = (uintptr_t)(void*)&tls_dtors_hook;
    {
        uint8_t* gw = tramp + HOOK_SIZE + ABS_JMP_GATEWAY_SIZE;
        gw[0] = 0xFF; gw[1] = 0x25; gw[2] = gw[3] = gw[4] = gw[5] = 0x00;  // jmp [rip]
        memcpy(gw + 6, &hook_addr, sizeof(hook_addr));
    }

    // 设置 C 可调用函数指针
    g_trampoline_fn = reinterpret_cast<void(*)(void)>(g_trampoline);

    // Step 4: 将目标页面改为可写
    uintptr_t page_start = (uintptr_t)real_call_tls_dtors_addr & ~0xFFFUL;
    if (mprotect((void*)page_start, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        const char err[] = "[FATAL] mprotect failed (need execmem?)\n";
        int ret = write(STDERR_FILENO, err, sizeof(err) - 1);
        return false;
    }

    // Step 5: 写入跳转到网关2（再由网关2绝对跳转到 hook 函数）的指令
    //   __call_tls_dtors 开头 --jmp rel32(5B)--> 网关2 --jmp abs--> tls_dtors_hook
    //   网关2在 trampoline 内存块内，距离必在 ±2GB 内，rel32 不会溢出
    uint8_t* gateway2 = tramp + HOOK_SIZE + ABS_JMP_GATEWAY_SIZE;
    uintptr_t patch_at  = (uintptr_t)real_call_tls_dtors_addr;
    int32_t gw2_rel32   = (int32_t)((uintptr_t)gateway2 - patch_at - 5);  // E9 jmp rel32 = 5 字节

    uint8_t patch[HOOK_SIZE];
    memset(patch, 0x90, HOOK_SIZE);  // NOP padding
    patch[0] = 0xE9;                  // jmp rel32
    memcpy(patch + 1, &gw2_rel32, 4);
    // 剩余字节用 NOP (0x90) 填充

    memcpy(real_call_tls_dtors_addr, patch, HOOK_SIZE);

    __builtin___clear_cache((char*)real_call_tls_dtors_addr,
                            (char*)real_call_tls_dtors_addr + HOOK_SIZE);

    return true;
}

// ====== 自动安装（利用构造函数在 main 之前执行）======

__attribute__((constructor))
static void init_hook(void) {
    if (!install_inline_hook()) {
        _exit(1);
    }
}

void reigster_hook_before_tls_dtor(void (*hook)(void))
{
    __hook_before_tls_dtor = hook;
}
