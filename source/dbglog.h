#ifndef DBGLOG_H
#define DBGLOG_H

// Crash-surviving debug log shared across main.c/ha_client.c - writes to
// sdmc:/3ds/ha3ds/ha3ds_log.txt, flushed after every line so a hard crash
// still leaves a trail on the SD card. Temporary - remove once the
// black-screen crash is tracked down.
void dbg_log_init(void);
void dbg_log(const char *fmt, ...);
void dbg_log_close(void);

#define LOG(...) dbg_log(__VA_ARGS__)

#endif
