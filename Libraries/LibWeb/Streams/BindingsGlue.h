/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::Streams {

class ReadableByteStreamController;
class ReadableStream;
class ReadableStreamDefaultController;
class TransformStream;
class TransformStreamDefaultController;
class WritableStream;
class WritableStreamDefaultController;

}

namespace Web::Bindings {

WEB_API WebIDL::ExceptionOr<JS::Value> invoke_readable_byte_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::ReadableByteStreamController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_readable_byte_stream_pull_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::ReadableByteStreamController>);
WEB_API Streams::ReadableStream* readable_stream_from_object(JS::Object&);
WEB_API void serialize_readable_stream_with_transfer(JS::Realm&, HTML::TransferDataEncoder&, GC::Ref<Streams::ReadableStream>);
WEB_API JS::Completion invoke_queuing_strategy_size_callback(WebIDL::CallbackType&, JS::Value);
WEB_API WebIDL::ExceptionOr<JS::Value> invoke_readable_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::ReadableStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_readable_stream_pull_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::ReadableStreamDefaultController>);
WEB_API void prepare_transform_stream_readable_writable_wrappers(JS::Realm&, Streams::TransformStream&);
WEB_API WebIDL::ExceptionOr<JS::Value> invoke_transform_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::TransformStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_transform_stream_transform_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, JS::Value, GC::Ref<Streams::TransformStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_transform_stream_flush_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::TransformStreamDefaultController>);
WEB_API Streams::WritableStream* writable_stream_from_object(JS::Object&);
WEB_API void serialize_writable_stream_with_transfer(JS::Realm&, HTML::TransferDataEncoder&, GC::Ref<Streams::WritableStream>);
WEB_API WebIDL::ExceptionOr<JS::Value> invoke_writable_stream_start_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, GC::Ref<Streams::WritableStreamDefaultController>);
WEB_API GC::Ref<WebIDL::Promise> invoke_writable_stream_write_algorithm_callback(JS::Realm&, WebIDL::CallbackType&, JS::Value, JS::Value, GC::Ref<Streams::WritableStreamDefaultController>);

}
