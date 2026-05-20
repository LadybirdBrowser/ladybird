/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if defined(HAS_ADDRESS_SANITIZER) && defined(AK_OS_LINUX)
extern "C" {

int __lsan_is_turned_off();
[[gnu::used]] int __lsan_is_turned_off()
{
    return 1;
}
}
#endif
