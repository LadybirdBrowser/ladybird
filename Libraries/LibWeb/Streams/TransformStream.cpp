/*
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>

namespace Web::Streams {

extern "C" {

// Defined in Bindings/Rust/src/streams/exports.rs.
void* web_rust_transform_stream_create(JSVM*, void* realm);
void web_rust_transform_stream_set_up(JSVM*, void* realm, void* stream, TransformAlgorithm* transform_algorithm, FlushAlgorithm* flush_algorithm, CancelAlgorithm* cancel_algorithm);
JSCompletion web_rust_transform_stream_enqueue(JSVM*, void* stream, JSValue chunk);
void* web_rust_transform_stream_readable(void* stream);
void* web_rust_transform_stream_writable(void* stream);
}

GC::Ref<TransformStream> TransformStream::create(JS::Realm& realm)
{
    return GC::Ref { *static_cast<TransformStream*>(web_rust_transform_stream_create(JS::HostABI::vm_to_abi(realm.vm()), &realm)) };
}

void TransformStream::set_up(JS::Realm& realm, GC::Ref<TransformAlgorithm> transform_algorithm, GC::Ptr<FlushAlgorithm> flush_algorithm, GC::Ptr<CancelAlgorithm> cancel_algorithm)
{
    web_rust_transform_stream_set_up(JS::HostABI::vm_to_abi(realm.vm()), &realm, this, transform_algorithm.ptr(), flush_algorithm.ptr(), cancel_algorithm.ptr());
}

WebIDL::ExceptionOr<void> TransformStream::enqueue(JS::Value chunk)
{
    auto result = JS::HostABI::completion_from_abi<JS::Value>(web_rust_transform_stream_enqueue(JS::HostABI::vm_to_abi(JS::VM::the()), this, JS::HostABI::value_to_abi(chunk)));
    if (result.is_error())
        return result.release_error();
    return {};
}

GC::Ref<ReadableStream> TransformStream::readable()
{
    return GC::Ref { *static_cast<ReadableStream*>(web_rust_transform_stream_readable(this)) };
}

GC::Ref<WritableStream> TransformStream::writable()
{
    return GC::Ref { *static_cast<WritableStream*>(web_rust_transform_stream_writable(this)) };
}

}
