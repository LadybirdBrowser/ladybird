/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#dictdef-underlyingsink
struct WEB_API UnderlyingSink {
    GC::Ptr<WebIDL::CallbackType> start;
    GC::Ptr<WebIDL::CallbackType> write;
    GC::Ptr<WebIDL::CallbackType> close;
    GC::Ptr<WebIDL::CallbackType> abort;
    Optional<JS::Value> type;

    static JS::ThrowCompletionOr<UnderlyingSink> from_value(JS::VM&, JS::Value);
};

}
