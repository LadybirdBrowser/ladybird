/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Streams/WritableStream.h>

namespace Web::Streams {

extern "C" {

// Defined in Bindings/Rust/src/streams/exports.rs.
void* web_rust_writable_stream_create(JSVM*, void* realm);
void web_rust_writable_stream_set_up(JSVM*, void* realm, void* stream, WriteAlgorithm* write_algorithm, double high_water_mark);
}

GC::Ref<WritableStream> WritableStream::create(JS::Realm& realm)
{
    return GC::Ref { *static_cast<WritableStream*>(web_rust_writable_stream_create(JS::HostABI::vm_to_abi(realm.vm()), &realm)) };
}

// https://streams.spec.whatwg.org/#writablestream-set-up
void WritableStream::set_up(JS::Realm& realm, GC::Ref<WriteAlgorithm> write_algorithm, double high_water_mark)
{
    web_rust_writable_stream_set_up(JS::HostABI::vm_to_abi(realm.vm()), &realm, this, write_algorithm.ptr(), high_water_mark);
}

}
