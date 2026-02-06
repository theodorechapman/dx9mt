#ifndef DX9MT_RUNTIME_H
#define DX9MT_RUNTIME_H

#include <stdint.h>

void dx9mt_runtime_ensure_initialized(void);
uint32_t dx9mt_runtime_next_packet_sequence(void);
void dx9mt_runtime_shutdown(void);

#endif
