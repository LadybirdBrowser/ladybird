/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ErrorTypes.h>

namespace JS {

#define __ENUMERATE_JS_ERROR(name, message) \
    const ErrorType ErrorType::name = ErrorType(message##sv);
JS_ENUMERATE_ERROR_TYPES(__ENUMERATE_JS_ERROR)
#undef __ENUMERATE_JS_ERROR

Utf16String const& ErrorType::message() const
{
    if (m_message.is_empty())
        m_message = Utf16String::from_utf8_without_validation(m_format);
    return m_message;
}

}
