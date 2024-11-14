/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#dictdef-transformer
struct Transformer {
    // https://streams.spec.whatwg.org/#dom-transformer-start
    GC::Root<WebIDL::CallbackType> start;
    // https://streams.spec.whatwg.org/#dom-transformer-transform
    GC::Root<WebIDL::CallbackType> transform;
    // https://streams.spec.whatwg.org/#dom-transformer-flush
    GC::Root<WebIDL::CallbackType> flush;
    // https://streams.spec.whatwg.org/#dom-transformer-cancel
    GC::Root<WebIDL::CallbackType> cancel;

    // https://streams.spec.whatwg.org/#dom-transformer-readabletype
    Optional<JS::Value> readable_type;
    // https://streams.spec.whatwg.org/#dom-transformer-writabletype
    Optional<JS::Value> writable_type;

    static JS::ThrowCompletionOr<Transformer> from_value(JS::VM&, JS::Value);
};

}
