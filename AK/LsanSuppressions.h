/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if defined(HAS_ADDRESS_SANITIZER)
#    include <sanitizer/lsan_interface.h>

extern "C" {
char const* __lsan_default_suppressions()
{
    // Both Skia and Chromium suppress false positive FontConfig leaks
    // https://github.com/google/skia/blob/main/tools/LsanSuppressions.cpp#L20
    // https://chromium.googlesource.com/chromium/src/build/+/master/sanitizers/lsan_suppressions.cc#25
    return "leak:FcPatternObjectInsertElt";
}
}
#endif

namespace AK {

inline void perform_leak_sanitizer_checks()
{
#if defined(HAS_ADDRESS_SANITIZER)
    __lsan_do_leak_check();
#endif
}

}
