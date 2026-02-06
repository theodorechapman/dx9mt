#ifndef DX9MT_LOG_H
#define DX9MT_LOG_H

void dx9mt_log_init(void);
void dx9mt_log_shutdown(void);
void dx9mt_logf(const char *tag, const char *fmt, ...);

#endif
