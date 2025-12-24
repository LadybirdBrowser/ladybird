/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#dictdef-transformer
struct WEB_API Transformer {
    // https://streams.spec.whatwg.org/#dom-transformer-start
    GC::Ptr<WebIDL::CallbackType> start;
    // https://streams.spec.whatwg.org/#dom-transformer-transform
    GC::Ptr<WebIDL::CallbackType> transform;
    // https://streams.spec.whatwg.org/#dom-transformer-flush
    GC::Ptr<WebIDL::CallbackType> flush;
    // https://streams.spec.whatwg.org/#dom-transformer-cancel
    GC::Ptr<WebIDL::CallbackType> cancel;

    // https://streams.spec.whatwg.org/#dom-transformer-readabletype
    Optional<JS::Value> readable_type;
    // https://streams.spec.whatwg.org/#dom-transformer-writabletype
    Optional<JS::Value> writable_type;

    static JS::ThrowCompletionOr<Transformer> from_value(JS::VM&, JS::Value);
};

}
