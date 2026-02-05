/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::Streams {

class GenericTransformStreamMixin {
public:
    virtual ~GenericTransformStreamMixin();

    GC::Ref<ReadableStream> readable();
    GC::Ref<WritableStream> writable();

protected:
    explicit GenericTransformStreamMixin(GC::Ref<TransformStream>);
    void visit_edges(GC::Cell::Visitor&);

    // https://streams.spec.whatwg.org/#generictransformstream-transform
    GC::Ref<TransformStream> m_transform;
};

}
