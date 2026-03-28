/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "JNIHelpers.h"
#include <AK/Utf16String.h>
#include <AK/Vector.h>

namespace Ladybird {

jstring JavaEnvironment::jstring_from_ak_string(String const& str)
{
    auto as_utf16 = AK::Utf16String::from_utf8(str);
    auto view = as_utf16.utf16_view();

    if (view.has_ascii_storage()) {
        // ascii_span is UTF-8, so we have to get it to UTF-16 for java
        auto ascii = view.ascii_span();
        Vector<jchar> widened;
        widened.ensure_capacity(ascii.size());
        for (size_t i = 0; i < ascii.size(); ++i)
            widened.unchecked_append(static_cast<jchar>(static_cast<unsigned char>(ascii[i])));
        return m_env->NewString(widened.data(), widened.size());
    }

    return m_env->NewString(reinterpret_cast<jchar const*>(view.utf16_span().data()), view.length_in_code_units());
}
}
