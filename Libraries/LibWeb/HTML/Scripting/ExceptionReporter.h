/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>

namespace Web::HTML {

enum class ErrorInPromise {
    No,
    Yes,
};

void report_exception_to_console(JS::Value, JS::Realm&, ErrorInPromise);
void report_exception(JS::Completion const&, JS::Realm&);

template<typename T>
inline void report_exception(JS::ThrowCompletionOr<T> const& result, JS::Realm& realm)
{
    VERIFY(result.is_throw_completion());
    report_exception(result.throw_completion(), realm);
}

}
