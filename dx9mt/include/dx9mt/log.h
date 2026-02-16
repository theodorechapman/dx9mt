#ifndef DX9MT_LOG_H
#define DX9MT_LOG_H

#include <stdlib.h>

void dx9mt_log_init(void);
void dx9mt_log_shutdown(void);
void dx9mt_logf(const char *tag, const char *fmt, ...);

/* Fatal: log message with file:line then abort. */
#define dx9mt_fatal(tag, ...) do { \
    dx9mt_logf(tag, "FATAL: " __VA_ARGS__); \
    dx9mt_logf(tag, "  at %s:%d", __FILE__, __LINE__); \
    abort(); \
} while (0)

#endif
