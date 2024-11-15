/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Streams/GenericTransformStream.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>

namespace Web::Streams {

GenericTransformStreamMixin::GenericTransformStreamMixin(GC::Ref<TransformStream> transform)
    : m_transform(transform)
{
}

GenericTransformStreamMixin::~GenericTransformStreamMixin() = default;

void GenericTransformStreamMixin::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_transform);
}

// https://streams.spec.whatwg.org/#dom-generictransformstream-readable
GC::Ref<ReadableStream> GenericTransformStreamMixin::readable()
{
    // The readable getter steps are to return this's transform.[[readable]].
    return m_transform->readable();
}

// https://streams.spec.whatwg.org/#dom-generictransformstream-writable
GC::Ref<WritableStream> GenericTransformStreamMixin::writable()
{
    // The writable getter steps are to return this's transform.[[writable]].
    return m_transform->writable();
}

}
