/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/ErrorInformation.h>

namespace Web::HTML {

class Script;

enum class ErrorInPromise {
    No,
    Yes,
};

enum class OmitError {
    No,
    Yes,
};

void report_exception_to_console(JS::Value, JS::Realm&, ErrorInPromise);
WEB_API void report_exception(JS::Completion const&, JS::Realm&);
void report_an_exception(JS::Object& global, JS::Value exception, OmitError = OmitError::No, GC::Ptr<Script> = {});
void report_an_exception(JS::Object& global, JS::Value exception, Optional<ErrorInformation>, OmitError = OmitError::No, GC::Ptr<Script> = {});

template<typename T>
inline void report_exception(JS::ThrowCompletionOr<T> const& result, JS::Realm& realm)
{
    VERIFY(result.is_throw_completion());
    report_exception(result.throw_completion(), realm);
}

}
