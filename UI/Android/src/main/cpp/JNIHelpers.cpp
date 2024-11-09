/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "JNIHelpers.h"
#include <AK/Utf16View.h>

namespace Ladybird {
jstring JavaEnvironment::jstring_from_ak_string(String const& str)
{
    auto as_utf16 = MUST(AK::utf8_to_utf16(str.code_points()));
    return m_env->NewString(as_utf16.data(), as_utf16.size());
}
}
