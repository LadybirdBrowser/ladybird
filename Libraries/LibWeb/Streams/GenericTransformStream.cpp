/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Streams/GenericTransformStream.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

// The transformAlgorithm and flushAlgorithm of a TransformStream that an implementation in Rust of an interface that
// includes GenericTransformStream sets up, which take the cell that holds them, and a chunk for the transformAlgorithm.
using RustTransformStreamSteps = JSCompletion (*)(JSVM*, JS::Realm*, GC::ForeignCell* steps, TransformStream*, JSValue const* chunk);

enum class TransformStreamSide : u8 {
    Readable,
    Writable,
};

extern "C" {
TransformStream* web_transform_stream_create(JS::Realm*, GC::ForeignCell* steps, RustTransformStreamSteps);
void web_transform_stream_enqueue(TransformStream*, JSValue chunk);
void* web_transform_stream_side(TransformStream*, JS::Realm*, TransformStreamSide);
}

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

static GC::Ref<WebIDL::Promise> promise_for(JS::Realm& realm, JSCompletion completion)
{
    auto result = JS::HostABI::completion_from_abi<JS::Value>(completion);
    if (result.is_error())
        return WebIDL::create_rejected_promise(realm, result.release_error().value());
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

TransformStream* web_transform_stream_create(JS::Realm* realm, GC::ForeignCell* steps, RustTransformStreamSteps run_steps)
{
    auto transform = GC::Heap::the().allocate<TransformStream>();

    auto transform_algorithm = GC::create_function(GC::Heap::the(), [realm = GC::Ref { *realm }, steps = GC::Ref { *steps }, transform, run_steps](JS::Value chunk) {
        auto abi_chunk = JS::HostABI::value_to_abi(chunk);
        return promise_for(realm, run_steps(JS::HostABI::vm_to_abi(realm->vm()), realm.ptr(), steps.ptr(), transform.ptr(), &abi_chunk));
    });
    auto flush_algorithm = GC::create_function(GC::Heap::the(), [realm = GC::Ref { *realm }, steps = GC::Ref { *steps }, transform, run_steps]() {
        return promise_for(realm, run_steps(JS::HostABI::vm_to_abi(realm->vm()), realm.ptr(), steps.ptr(), transform.ptr(), nullptr));
    });
    transform->set_up(*realm, transform_algorithm, flush_algorithm);

    return transform.ptr();
}

void web_transform_stream_enqueue(TransformStream* transform, JSValue chunk)
{
    transform->enqueue(JS::HostABI::value_from_abi(chunk));
}

void* web_transform_stream_side(TransformStream* transform, JS::Realm* realm, TransformStreamSide side)
{
    GC::Ref<Bindings::Wrappable> stream = side == TransformStreamSide::Readable
        ? GC::Ref<Bindings::Wrappable> { transform->readable() }
        : GC::Ref<Bindings::Wrappable> { transform->writable() };
    return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, stream).ptr();
}

}
