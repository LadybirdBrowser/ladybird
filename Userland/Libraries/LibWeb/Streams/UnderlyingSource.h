/*
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

enum class ReadableStreamType {
    Bytes
};

struct UnderlyingSource {
    GC::Handle<WebIDL::CallbackType> start;
    GC::Handle<WebIDL::CallbackType> pull;
    GC::Handle<WebIDL::CallbackType> cancel;
    Optional<ReadableStreamType> type;
    Optional<u64> auto_allocate_chunk_size;

    static JS::ThrowCompletionOr<UnderlyingSource> from_value(JS::VM&, JS::Value);
};

}
