/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/Algorithms.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

// 4.9.1. Working with readable streams, https://streams.spec.whatwg.org/#rs-abstract-ops
WebIDL::ExceptionOr<GC::Ref<ReadableStreamBYOBReader>> acquire_readable_stream_byob_reader(ReadableStream&);
WebIDL::ExceptionOr<GC::Ref<ReadableStreamDefaultReader>> acquire_readable_stream_default_reader(ReadableStream&);
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> create_readable_stream(JS::Realm& realm, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark = {}, GC::Ptr<SizeAlgorithm> size_algorithm = {});
WebIDL::ExceptionOr<GC::Ref<ReadableStream>> create_readable_byte_stream(JS::Realm& realm, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<PullAlgorithm> pull_algorithm, GC::Ref<CancelAlgorithm> cancel_algorithm);
void initialize_readable_stream(ReadableStream&);
bool is_readable_stream_locked(ReadableStream const&);

WebIDL::ExceptionOr<GC::Ref<ReadableStream>> readable_stream_from_iterable(JS::VM& vm, JS::Value async_iterable);
GC::Ref<WebIDL::Promise> readable_stream_pipe_to(ReadableStream& source, WritableStream& dest, bool prevent_close, bool prevent_abort, bool prevent_cancel, GC::Ptr<DOM::AbortSignal> signal = {});
WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_tee(JS::Realm&, ReadableStream&, bool clone_for_branch2);
WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_default_tee(JS::Realm& realm, ReadableStream& stream, bool clone_for_branch2);
WebIDL::ExceptionOr<ReadableStreamPair> readable_byte_stream_tee(JS::Realm& realm, ReadableStream& stream);

// 4.9.2. Interfacing with controllers, https://streams.spec.whatwg.org/#rs-abstract-ops-used-by-controllers
void readable_stream_add_read_into_request(ReadableStream&, GC::Ref<ReadIntoRequest>);
void readable_stream_add_read_request(ReadableStream&, GC::Ref<ReadRequest>);
GC::Ref<WebIDL::Promise> readable_stream_cancel(ReadableStream&, JS::Value reason);
void readable_stream_close(ReadableStream&);
void readable_stream_error(ReadableStream&, JS::Value error);

void readable_stream_fulfill_read_into_request(ReadableStream&, JS::Value chunk, bool done);
void readable_stream_fulfill_read_request(ReadableStream&, JS::Value chunk, bool done);
size_t readable_stream_get_num_read_into_requests(ReadableStream const&);
size_t readable_stream_get_num_read_requests(ReadableStream const&);
bool readable_stream_has_byob_reader(ReadableStream const&);
bool readable_stream_has_default_reader(ReadableStream const&);

// 4.9.3. Readers, https://streams.spec.whatwg.org/#rs-reader-abstract-ops
GC::Ref<WebIDL::Promise> readable_stream_reader_generic_cancel(ReadableStreamGenericReaderMixin&, JS::Value reason);
void readable_stream_reader_generic_initialize(ReadableStreamReader const&, ReadableStream&);
void readable_stream_reader_generic_release(ReadableStreamGenericReaderMixin&);

void readable_stream_byob_reader_error_read_into_requests(ReadableStreamBYOBReader&, JS::Value error);
void readable_stream_byob_reader_read(ReadableStreamBYOBReader&, WebIDL::ArrayBufferView&, u64 min, ReadIntoRequest&);
void readable_stream_byob_reader_release(ReadableStreamBYOBReader&);

void readable_stream_default_reader_error_read_requests(ReadableStreamDefaultReader&, JS::Value error);
void readable_stream_default_reader_read(ReadableStreamDefaultReader&, ReadRequest&);
void readable_stream_default_reader_release(ReadableStreamDefaultReader&);

WebIDL::ExceptionOr<void> set_up_readable_stream_byob_reader(ReadableStreamBYOBReader&, ReadableStream&);
WebIDL::ExceptionOr<void> set_up_readable_stream_default_reader(ReadableStreamDefaultReader&, ReadableStream&);

// 4.9.4. Default controllers, https://streams.spec.whatwg.org/#rs-default-controller-abstract-ops
void readable_stream_default_controller_call_pull_if_needed(ReadableStreamDefaultController&);
bool readable_stream_default_controller_should_call_pull(ReadableStreamDefaultController&);
void readable_stream_default_controller_clear_algorithms(ReadableStreamDefaultController&);
void readable_stream_default_controller_close(ReadableStreamDefaultController&);
WebIDL::ExceptionOr<void> readable_stream_default_controller_enqueue(ReadableStreamDefaultController&, JS::Value chunk);
void readable_stream_default_controller_error(ReadableStreamDefaultController&, JS::Value error);
Optional<double> readable_stream_default_controller_get_desired_size(ReadableStreamDefaultController&);
bool readable_stream_default_controller_has_backpressure(ReadableStreamDefaultController&);
bool readable_stream_default_controller_can_close_or_enqueue(ReadableStreamDefaultController&);
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller(ReadableStream&, ReadableStreamDefaultController&, GC::Ref<StartAlgorithm>, GC::Ref<PullAlgorithm>, GC::Ref<CancelAlgorithm>, double high_water_mark, GC::Ref<SizeAlgorithm>);
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller_from_underlying_source(ReadableStream&, JS::Value underlying_source_value, UnderlyingSource const&, double high_water_mark, GC::Ref<SizeAlgorithm>);

// 4.9.5. Byte stream controllers, https://streams.spec.whatwg.org/#rbs-controller-abstract-ops
void readable_byte_stream_controller_call_pull_if_needed(ReadableByteStreamController&);
void readable_byte_stream_controller_clear_algorithms(ReadableByteStreamController&);
void readable_byte_stream_controller_clear_pending_pull_intos(ReadableByteStreamController&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_close(ReadableByteStreamController&);
void readable_byte_stream_controller_commit_pull_into_descriptor(ReadableStream&, PullIntoDescriptor const&);
JS::Value readable_byte_stream_controller_convert_pull_into_descriptor(JS::Realm&, PullIntoDescriptor const&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue(ReadableByteStreamController& controller, JS::Value chunk);
void readable_byte_stream_controller_enqueue_chunk_to_queue(ReadableByteStreamController& controller, GC::Ref<JS::ArrayBuffer> buffer, u32 byte_offset, u32 byte_length);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(ReadableByteStreamController& controller, JS::ArrayBuffer& buffer, u64 byte_offset, u64 byte_length);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_detached_pull_into_to_queue(ReadableByteStreamController& controller, PullIntoDescriptor& pull_into_descriptor);
void readable_byte_stream_controller_error(ReadableByteStreamController&, JS::Value error);
void readable_byte_stream_controller_fill_head_pull_into_descriptor(ReadableByteStreamController const&, u64 size, PullIntoDescriptor&);
bool readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(ReadableByteStreamController&, PullIntoDescriptor&);
void readable_byte_stream_controller_fill_read_request_from_queue(ReadableByteStreamController&, ReadRequest&);
GC::Ptr<ReadableStreamBYOBRequest> readable_byte_stream_controller_get_byob_request(ReadableByteStreamController&);
Optional<double> readable_byte_stream_controller_get_desired_size(ReadableByteStreamController const&);
void readable_byte_stream_controller_handle_queue_drain(ReadableByteStreamController&);
void readable_byte_stream_controller_invalidate_byob_request(ReadableByteStreamController&);
[[nodiscard]] SinglyLinkedList<GC::Root<PullIntoDescriptor>> readable_byte_stream_controller_process_pull_into_descriptors_using_queue(ReadableByteStreamController&);
void readable_byte_stream_controller_process_read_requests_using_queue(ReadableByteStreamController& controller);
void readable_byte_stream_controller_pull_into(ReadableByteStreamController&, WebIDL::ArrayBufferView&, u64 min, ReadIntoRequest&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond(ReadableByteStreamController&, u64 bytes_written);
void readable_byte_stream_controller_respond_in_closed_state(ReadableByteStreamController&, PullIntoDescriptor&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_in_readable_state(ReadableByteStreamController&, u64 bytes_written, PullIntoDescriptor&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_internal(ReadableByteStreamController&, u64 bytes_written);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_with_new_view(JS::Realm&, ReadableByteStreamController&, WebIDL::ArrayBufferView&);
GC::Ref<PullIntoDescriptor> readable_byte_stream_controller_shift_pending_pull_into(ReadableByteStreamController& controller);
bool readable_byte_stream_controller_should_call_pull(ReadableByteStreamController const&);
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller(ReadableStream&, ReadableByteStreamController&, GC::Ref<StartAlgorithm>, GC::Ref<PullAlgorithm>, GC::Ref<CancelAlgorithm>, double high_water_mark, JS::Value auto_allocate_chunk_size);
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller_from_underlying_source(ReadableStream&, JS::Value underlying_source, UnderlyingSource const& underlying_source_dict, double high_water_mark);

}
