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

}
