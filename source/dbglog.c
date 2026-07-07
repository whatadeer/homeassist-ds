#include "dbglog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

static FILE *g_log = NULL;
static LightLock g_log_lock;

void dbg_log_init(void) {
    LightLock_Init(&g_log_lock);
    g_log = fopen("sdmc:/ha3ds_log.txt", "w");
}

void dbg_log(const char *fmt, ...) {
    if (!g_log) {
        return;
    }

    // Called from both the main thread and the network worker thread.
    LightLock_Lock(&g_log_lock);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);

    fprintf(g_log, "\n");
    fflush(g_log);

    LightLock_Unlock(&g_log_lock);
}

void dbg_log_close(void) {
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }
}
