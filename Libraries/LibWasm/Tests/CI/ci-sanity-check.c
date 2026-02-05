#include <emscripten.h>
#include <stdio.h>

// Compiled with: -O3 -g0

EMSCRIPTEN_KEEPALIVE
void run_sanity_check() {
    printf("Hello, World!\n");
}
