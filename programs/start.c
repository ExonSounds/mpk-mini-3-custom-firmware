/* Start-up for RAM programs: the header the firmware looks for, bss zeroing, and the memcpy/memset the compiler
 * may call. Your program provides: int program(const prog_api_t *api); */
#include <stddef.h>
#include "prog_api.h"

extern uint32_t _sbss[], _ebss[];
int program(const prog_api_t *api);

static int start(const prog_api_t *api) {
    for (uint32_t *p = _sbss; p < _ebss; p++) *p = 0;
    return program(api);
}

__attribute__((section(".prog_header"), used)) const prog_header_t prog_header = {PROG_MAGIC, start};

void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d;
    const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t)c;
    return d;
}
void __aeabi_memcpy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { memset(d, 0, n); }
void __aeabi_memset(void *d, size_t n, int c) { memset(d, c, n); }
