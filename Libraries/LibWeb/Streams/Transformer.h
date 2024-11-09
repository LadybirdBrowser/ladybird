/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Heap/Handle.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#dictdef-transformer
struct Transformer {
    // https://streams.spec.whatwg.org/#dom-transformer-start
    JS::Handle<WebIDL::CallbackType> start;
    // https://streams.spec.whatwg.org/#dom-transformer-transform
    JS::Handle<WebIDL::CallbackType> transform;
    // https://streams.spec.whatwg.org/#dom-transformer-flush
    JS::Handle<WebIDL::CallbackType> flush;
    // https://streams.spec.whatwg.org/#dom-transformer-cancel
    JS::Handle<WebIDL::CallbackType> cancel;

    // https://streams.spec.whatwg.org/#dom-transformer-readabletype
    Optional<JS::Value> readable_type;
    // https://streams.spec.whatwg.org/#dom-transformer-writabletype
    Optional<JS::Value> writable_type;

    static JS::ThrowCompletionOr<Transformer> from_value(JS::VM&, JS::Value);
};

}
