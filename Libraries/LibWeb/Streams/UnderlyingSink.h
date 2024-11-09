/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#dictdef-underlyingsink
struct UnderlyingSink {
    JS::Handle<WebIDL::CallbackType> start;
    JS::Handle<WebIDL::CallbackType> write;
    JS::Handle<WebIDL::CallbackType> close;
    JS::Handle<WebIDL::CallbackType> abort;
    Optional<JS::Value> type;

    static JS::ThrowCompletionOr<UnderlyingSink> from_value(JS::VM&, JS::Value);
};

}
