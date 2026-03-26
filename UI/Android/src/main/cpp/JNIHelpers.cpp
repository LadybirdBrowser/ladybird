/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "JNIHelpers.h"
#include <AK/Utf16String.h>

namespace Ladybird {

jstring JavaEnvironment::jstring_from_ak_string(String const& str)
{

auto as_utf16 = AK::Utf16String::from_utf8(str);
auto view = as_utf16.utf16_view();
if (view.has_ascii_storage())
    return m_env->NewStringUTF(view.ascii_span().data());
return m_env->NewString(reinterpret_cast<jchar const*>(view.utf16_span().data()), view.length_in_code_units());
}
}
