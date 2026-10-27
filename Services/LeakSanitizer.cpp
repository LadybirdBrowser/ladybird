/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if defined(HAS_LEAK_SANITIZER)
// LeakSanitizer's exit check needs syscalls denied by the sandbox. Its hook must return a constant,
// so these helper executables also skip leak checks when launched with --disable-sandbox.
extern "C" int __lsan_is_turned_off();
extern "C" [[gnu::used]] int __lsan_is_turned_off()
{
    return 1;
}
#endif
