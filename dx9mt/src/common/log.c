#include "dx9mt/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static FILE *g_log_stream;
static int g_log_ready;

static void dx9mt_log_write_line(const char *line) {
  if (!g_log_stream) {
    return;
  }

  fputs(line, g_log_stream);
  fputc('\n', g_log_stream);
  fflush(g_log_stream);

#if defined(_WIN32)
  OutputDebugStringA(line);
  OutputDebugStringA("\n");
#endif
}

void dx9mt_log_init(void) {
  const char *path;

  if (g_log_ready) {
    return;
  }

  g_log_stream = stderr;

  path = getenv("DX9MT_LOG_PATH");
  if (path && path[0] != '\0') {
    FILE *file = fopen(path, "a");
    if (file) {
      g_log_stream = file;
    }
  }

  g_log_ready = 1;
  dx9mt_logf("log", "initialized");
}

void dx9mt_log_shutdown(void) {
  if (!g_log_ready) {
    return;
  }

  dx9mt_logf("log", "shutdown");

  if (g_log_stream && g_log_stream != stderr) {
    fclose(g_log_stream);
  }

  g_log_stream = NULL;
  g_log_ready = 0;
}

void dx9mt_logf(const char *tag, const char *fmt, ...) {
  char timebuf[32];
  char msgbuf[1024];
  char linebuf[1200];
  va_list ap;
  time_t now;
#if defined(_WIN32)
  DWORD tid = GetCurrentThreadId();
#else
  unsigned long tid = 0;
#endif

  if (!g_log_ready) {
    dx9mt_log_init();
  }

  now = time(NULL);
  if (!strftime(timebuf, sizeof(timebuf), "%H:%M:%S", localtime(&now))) {
    strcpy(timebuf, "00:00:00");
  }

  va_start(ap, fmt);
  vsnprintf(msgbuf, sizeof(msgbuf), fmt, ap);
  va_end(ap);

  snprintf(linebuf, sizeof(linebuf), "[%s] [tid=%04lx] dx9mt/%s: %s", timebuf,
           (unsigned long)tid, tag ? tag : "core", msgbuf);
  dx9mt_log_write_line(linebuf);
}
