#include "dbglog.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

// Same /3ds/ha3ds/ folder app_config.c uses - see its DATA_DIR comment for
// why this doesn't just live at the SD card root.
#define LOG_PATH "sdmc:/3ds/ha3ds/ha3ds_log.txt"

static FILE *g_log = NULL;
static LightLock g_log_lock;

void dbg_log_init(void) {
    LightLock_Init(&g_log_lock);
    // fopen() never creates missing parent directories - app_config_save()
    // usually creates sdmc:/3ds/ha3ds first, but logging starts before any
    // config save happens, so this can't assume that's already run.
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/ha3ds", 0777);
    g_log = fopen(LOG_PATH, "w");
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
