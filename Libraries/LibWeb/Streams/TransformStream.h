/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/TransformStream.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/Algorithms.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

// https://streams.spec.whatwg.org/#transformstream
// The C++ side of a TransformStream, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/streams): what
// the specifications that LibWeb implements in C++ use to set up transform streams,
// https://streams.spec.whatwg.org/#other-specs-ts
class WEB_API TransformStream : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(TransformStream, transform_stream)

public:
    // A new TransformStream in the realm, which set_up() then sets up.
    [[nodiscard]] static GC::Ref<TransformStream> create(JS::Realm&);

    // https://streams.spec.whatwg.org/#transformstream-set-up
    void set_up(JS::Realm&, GC::Ref<TransformAlgorithm>, GC::Ptr<FlushAlgorithm> = {}, GC::Ptr<CancelAlgorithm> = {});

    // https://streams.spec.whatwg.org/#transformstream-enqueue
    WebIDL::ExceptionOr<void> enqueue(JS::Value chunk);

    // https://streams.spec.whatwg.org/#transformstream-readable
    GC::Ref<ReadableStream> readable();

    // https://streams.spec.whatwg.org/#transformstream-writable
    GC::Ref<WritableStream> writable();
};

}
