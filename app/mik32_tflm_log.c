#include "xprintf.h"

void xputc(char c);

void Mik32DebugLogWrite(const char* s) {
    if (s == 0) {
        return;
    }
    xputs(s);
    xputc('\r');
    xputc('\n');
}
