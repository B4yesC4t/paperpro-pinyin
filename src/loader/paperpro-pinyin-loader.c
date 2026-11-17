// PaperPro 拼音：加载器（预加载进 xochitl，放在系统分区）。
//
// 输入法模块装在 /home 里，而 /home 是加密分区：开机时 xochitl 往往比它先启动
// （reMarkable 有意如此，xochitl 会在进程内等 /home 就绪）。直接预加载 /home 里的
// 模块，就要看谁先谁后；输了，这次开机就没有输入法。
//
// 这个加载器只依赖 C 运行库，放在 / 上，总能被预加载。它：
//   - /home 已就绪：立即加载模块，和直接预加载完全一样；
//   - 还没就绪：开一个线程每 50 ms 看一次（最多 2 分钟），模块文件出现后再加载，
//     并请模块自己补上启动（paperpro_pinyin_late_start）。
// 链接时带上 Qt 的库（不调用其中的函数），使它在 Qt 初始化之后才运行，与原先直接
// 预加载模块时的初始化顺序一致。
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define MODULE "/home/root/paperpro-pinyin/libpaperpro-pinyin.so"

static int is_xochitl(void) {
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n < 0) return 0;
    exe[n] = 0;
    return strcmp(exe, "/usr/bin/xochitl") == 0;
}

static void load(int late) {
    void *h = dlopen(MODULE, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[paperpro-pinyin] loader: %s\n", dlerror());
        return;
    }
    if (late) {
        void (*start)(void) = (void (*)(void))dlsym(h, "paperpro_pinyin_late_start");
        if (start) start();
    }
}

static void *wait_for_home(void *arg) {
    (void)arg;
    for (int ms = 0; ms <= 120000; ms += 50) {
        if (access(MODULE, R_OK) == 0) {
            fprintf(stderr, "[paperpro-pinyin] loader: module available after %d ms\n", ms);
            load(1);
            return NULL;
        }
        usleep(50000);
    }
    fprintf(stderr, "[paperpro-pinyin] loader: %s did not appear within 2 minutes\n", MODULE);
    return NULL;
}

__attribute__((constructor)) static void init(void) {
    if (!is_xochitl()) return;
    if (access(MODULE, R_OK) == 0) {
        load(0);
        return;
    }
    fprintf(stderr, "[paperpro-pinyin] loader: /home not ready yet, waiting for it\n");
    pthread_attr_t a;
    pthread_t t;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, wait_for_home, NULL) != 0)
        fprintf(stderr, "[paperpro-pinyin] loader: cannot start waiting thread\n");
    pthread_attr_destroy(&a);
}
