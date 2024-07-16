/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Promise.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Streams {

using SizeAlgorithm = JS::HeapFunction<JS::Completion(JS::Value)>;
using PullAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>()>;
using CancelAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>(JS::Value)>;
using StartAlgorithm = JS::HeapFunction<WebIDL::ExceptionOr<JS::Value>()>;
using AbortAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>(JS::Value)>;
using CloseAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>()>;
using WriteAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>(JS::Value)>;
using FlushAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>()>;
using TransformAlgorithm = JS::HeapFunction<JS::NonnullGCPtr<WebIDL::Promise>(JS::Value)>;

WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStreamDefaultReader>> acquire_readable_stream_default_reader(ReadableStream&);
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStreamBYOBReader>> acquire_readable_stream_byob_reader(ReadableStream&);
bool is_readable_stream_locked(ReadableStream const&);

JS::NonnullGCPtr<SizeAlgorithm> extract_size_algorithm(JS::VM&, QueuingStrategy const&);
WebIDL::ExceptionOr<double> extract_high_water_mark(QueuingStrategy const&, double default_hwm);

void readable_stream_close(ReadableStream&);
void readable_stream_error(ReadableStream&, JS::Value error);
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> readable_stream_from_iterable(JS::VM& vm, JS::Value async_iterable);
void readable_stream_add_read_request(ReadableStream&, JS::NonnullGCPtr<ReadRequest>);
void readable_stream_add_read_into_request(ReadableStream&, JS::NonnullGCPtr<ReadIntoRequest>);
JS::NonnullGCPtr<WebIDL::Promise> readable_stream_cancel(ReadableStream&, JS::Value reason);
void readable_stream_fulfill_read_into_request(ReadableStream&, JS::Value chunk, bool done);
void readable_stream_fulfill_read_request(ReadableStream&, JS::Value chunk, bool done);
size_t readable_stream_get_num_read_into_requests(ReadableStream const&);
size_t readable_stream_get_num_read_requests(ReadableStream const&);
bool readable_stream_has_byob_reader(ReadableStream const&);
bool readable_stream_has_default_reader(ReadableStream const&);

JS::NonnullGCPtr<WebIDL::Promise> readable_stream_pipe_to(ReadableStream& source, WritableStream& dest, bool prevent_close, bool prevent_abort, bool prevent_cancel, Optional<JS::Value> signal);

WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_tee(JS::Realm&, ReadableStream&, bool clone_for_branch2);
WebIDL::ExceptionOr<ReadableStreamPair> readable_stream_default_tee(JS::Realm& realm, ReadableStream& stream, bool clone_for_branch2);
WebIDL::ExceptionOr<ReadableStreamPair> readable_byte_stream_tee(JS::Realm& realm, ReadableStream& stream);

JS::NonnullGCPtr<WebIDL::Promise> readable_stream_reader_generic_cancel(ReadableStreamGenericReaderMixin&, JS::Value reason);
void readable_stream_reader_generic_initialize(ReadableStreamReader, ReadableStream&);
void readable_stream_reader_generic_release(ReadableStreamGenericReaderMixin&);

void readable_stream_default_reader_error_read_requests(ReadableStreamDefaultReader&, JS::Value error);
void readable_stream_byob_reader_error_read_into_requests(ReadableStreamBYOBReader&, JS::Value error);
JS::Value readable_byte_stream_controller_convert_pull_into_descriptor(JS::Realm&, PullIntoDescriptor const&);
void readable_byte_stream_controller_pull_into(ReadableByteStreamController&, WebIDL::ArrayBufferView&, u64 min, ReadIntoRequest&);
void readable_stream_byob_reader_read(ReadableStreamBYOBReader&, WebIDL::ArrayBufferView&, u64 min, ReadIntoRequest&);
void readable_byte_stream_controller_fill_head_pull_into_descriptor(ReadableByteStreamController const&, u64 size, PullIntoDescriptor&);

void readable_stream_default_reader_read(ReadableStreamDefaultReader&, ReadRequest&);
void readable_stream_default_reader_release(ReadableStreamDefaultReader&);
void readable_stream_byob_reader_release(ReadableStreamBYOBReader&);
WebIDL::ExceptionOr<void> set_up_readable_stream_default_reader(ReadableStreamDefaultReader&, ReadableStream&);
WebIDL::ExceptionOr<void> set_up_readable_stream_byob_reader(ReadableStreamBYOBReader&, ReadableStream&);
void readable_stream_default_controller_close(ReadableStreamDefaultController&);
bool readable_stream_default_controller_has_backpressure(ReadableStreamDefaultController&);
WebIDL::ExceptionOr<void> readable_stream_default_controller_enqueue(ReadableStreamDefaultController&, JS::Value chunk);
void readable_stream_default_controller_can_pull_if_needed(ReadableStreamDefaultController&);
bool readable_stream_default_controller_should_call_pull(ReadableStreamDefaultController&);
void readable_stream_default_controller_clear_algorithms(ReadableStreamDefaultController&);

void readable_stream_default_controller_error(ReadableStreamDefaultController&, JS::Value error);
Optional<double> readable_stream_default_controller_get_desired_size(ReadableStreamDefaultController&);
bool readable_stream_default_controller_can_close_or_enqueue(ReadableStreamDefaultController&);
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller(ReadableStream&, ReadableStreamDefaultController&, JS::NonnullGCPtr<StartAlgorithm>, JS::NonnullGCPtr<PullAlgorithm>, JS::NonnullGCPtr<CancelAlgorithm>, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm>);
WebIDL::ExceptionOr<void> set_up_readable_stream_default_controller_from_underlying_source(ReadableStream&, JS::Value underlying_source_value, UnderlyingSource, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm>);
void set_up_readable_stream_controller_with_byte_reading_support(ReadableStream&, JS::GCPtr<PullAlgorithm> = {}, JS::GCPtr<CancelAlgorithm> = {}, double high_water_mark = 0);
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller(ReadableStream&, ReadableByteStreamController&, JS::NonnullGCPtr<StartAlgorithm>, JS::NonnullGCPtr<PullAlgorithm>, JS::NonnullGCPtr<CancelAlgorithm>, double high_water_mark, JS::Value auto_allocate_chunk_size);
WebIDL::ExceptionOr<void> set_up_readable_byte_stream_controller_from_underlying_source(ReadableStream&, JS::Value underlying_source, UnderlyingSource const& underlying_source_dict, double high_water_mark);
JS::GCPtr<ReadableStreamBYOBRequest> readable_byte_stream_controller_get_byob_request(JS::NonnullGCPtr<ReadableByteStreamController>);

WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_in_readable_state(ReadableByteStreamController&, u64 bytes_written, PullIntoDescriptor&);
void readable_byte_stream_controller_respond_in_closed_state(ReadableByteStreamController&, PullIntoDescriptor&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_internal(ReadableByteStreamController&, u64 bytes_written);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond(ReadableByteStreamController&, u64 bytes_written);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_respond_with_new_view(JS::Realm&, ReadableByteStreamController&, WebIDL::ArrayBufferView&);

WebIDL::ExceptionOr<void> readable_stream_enqueue(ReadableStreamController& controller, JS::Value chunk);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue(ReadableByteStreamController& controller, JS::Value chunk);
WebIDL::ExceptionOr<void> readable_stream_pull_from_bytes(ReadableStream&, ByteBuffer bytes);
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::ArrayBuffer>> transfer_array_buffer(JS::Realm& realm, JS::ArrayBuffer& buffer);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_detached_pull_into_queue(ReadableByteStreamController& controller, PullIntoDescriptor& pull_into_descriptor);
void readable_byte_stream_controller_commit_pull_into_descriptor(ReadableStream&, PullIntoDescriptor const&);
void readable_byte_stream_controller_process_read_requests_using_queue(ReadableByteStreamController& controller);
void readable_byte_stream_controller_process_pull_into_descriptors_using_queue(ReadableByteStreamController&);
void readable_byte_stream_controller_enqueue_chunk_to_queue(ReadableByteStreamController& controller, JS::NonnullGCPtr<JS::ArrayBuffer> buffer, u32 byte_offset, u32 byte_length);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_enqueue_cloned_chunk_to_queue(ReadableByteStreamController& controller, JS::ArrayBuffer& buffer, u64 byte_offset, u64 byte_length);
PullIntoDescriptor readable_byte_stream_controller_shift_pending_pull_into(ReadableByteStreamController& controller);

void readable_byte_stream_controller_call_pull_if_needed(ReadableByteStreamController&);
void readable_byte_stream_controller_clear_algorithms(ReadableByteStreamController&);
void readable_byte_stream_controller_clear_pending_pull_intos(ReadableByteStreamController&);
WebIDL::ExceptionOr<void> readable_byte_stream_controller_close(ReadableByteStreamController&);
void readable_byte_stream_controller_error(ReadableByteStreamController&, JS::Value error);
void readable_byte_stream_controller_fill_read_request_from_queue(ReadableByteStreamController&, JS::NonnullGCPtr<ReadRequest>);
bool readable_byte_stream_controller_fill_pull_into_descriptor_from_queue(ReadableByteStreamController&, PullIntoDescriptor&);
Optional<double> readable_byte_stream_controller_get_desired_size(ReadableByteStreamController const&);
void readable_byte_stream_controller_handle_queue_drain(ReadableByteStreamController&);
void readable_byte_stream_controller_invalidate_byob_request(ReadableByteStreamController&);
bool readable_byte_stream_controller_should_call_pull(ReadableByteStreamController const&);

WebIDL::ExceptionOr<void> set_up_readable_stream(JS::Realm& realm, ReadableStream& stream, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark = {}, JS::GCPtr<SizeAlgorithm> size_algorithm = {});
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> create_readable_stream(JS::Realm& realm, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm, Optional<double> high_water_mark = {}, JS::GCPtr<SizeAlgorithm> size_algorithm = {});
WebIDL::ExceptionOr<JS::NonnullGCPtr<ReadableStream>> create_readable_byte_stream(JS::Realm& realm, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<PullAlgorithm> pull_algorithm, JS::NonnullGCPtr<CancelAlgorithm> cancel_algorithm);
WebIDL::ExceptionOr<JS::NonnullGCPtr<WritableStream>> create_writable_stream(JS::Realm& realm, JS::NonnullGCPtr<StartAlgorithm> start_algorithm, JS::NonnullGCPtr<WriteAlgorithm> write_algorithm, JS::NonnullGCPtr<CloseAlgorithm> close_algorithm, JS::NonnullGCPtr<AbortAlgorithm> abort_algorithm, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm);
void initialize_readable_stream(ReadableStream&);
void initialize_writable_stream(WritableStream&);

WebIDL::ExceptionOr<JS::NonnullGCPtr<WritableStreamDefaultWriter>> acquire_writable_stream_default_writer(WritableStream&);
bool is_writable_stream_locked(WritableStream const&);
WebIDL::ExceptionOr<void> set_up_writable_stream_default_writer(WritableStreamDefaultWriter&, WritableStream&);
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_abort(WritableStream&, JS::Value reason);
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_close(WritableStream&);

JS::NonnullGCPtr<WebIDL::Promise> writable_stream_add_write_request(WritableStream&);
bool writable_stream_close_queued_or_in_flight(WritableStream const&);
void writable_stream_deal_with_rejection(WritableStream&, JS::Value error);
void writable_stream_finish_erroring(WritableStream&);
void writable_stream_finish_in_flight_close(WritableStream&);
void writable_stream_finish_in_flight_close_with_error(WritableStream&, JS::Value error);
void writable_stream_finish_in_flight_write(WritableStream&);
void writable_stream_finish_in_flight_write_with_error(WritableStream&, JS::Value error);
bool writable_stream_has_operation_marked_in_flight(WritableStream const&);
void writable_stream_mark_close_request_in_flight(WritableStream&);
void writable_stream_mark_first_write_request_in_flight(WritableStream&);
void writable_stream_reject_close_and_closed_promise_if_needed(WritableStream&);
void writable_stream_start_erroring(WritableStream&, JS::Value reason);
void writable_stream_update_backpressure(WritableStream&, bool backpressure);

JS::NonnullGCPtr<WebIDL::Promise> writable_stream_default_writer_abort(WritableStreamDefaultWriter&, JS::Value reason);
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_default_writer_close(WritableStreamDefaultWriter&);
void writable_stream_default_writer_ensure_closed_promise_rejected(WritableStreamDefaultWriter&, JS::Value error);
void writable_stream_default_writer_ensure_ready_promise_rejected(WritableStreamDefaultWriter&, JS::Value error);
Optional<double> writable_stream_default_writer_get_desired_size(WritableStreamDefaultWriter const&);
void writable_stream_default_writer_release(WritableStreamDefaultWriter&);
JS::NonnullGCPtr<WebIDL::Promise> writable_stream_default_writer_write(WritableStreamDefaultWriter&, JS::Value chunk);

WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller(WritableStream&, WritableStreamDefaultController&, JS::NonnullGCPtr<StartAlgorithm>, JS::NonnullGCPtr<WriteAlgorithm>, JS::NonnullGCPtr<CloseAlgorithm>, JS::NonnullGCPtr<AbortAlgorithm>, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm>);
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller_from_underlying_sink(WritableStream&, JS::Value underlying_sink_value, UnderlyingSink&, double high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> size_algorithm);
void writable_stream_default_controller_advance_queue_if_needed(WritableStreamDefaultController&);
void writable_stream_default_controller_clear_algorithms(WritableStreamDefaultController&);
void writable_stream_default_controller_close(WritableStreamDefaultController&);
void writable_stream_default_controller_error(WritableStreamDefaultController&, JS::Value error);
void writable_stream_default_controller_error_if_needed(WritableStreamDefaultController&, JS::Value error);
bool writable_stream_default_controller_get_backpressure(WritableStreamDefaultController const&);
JS::Value writable_stream_default_controller_get_chunk_size(WritableStreamDefaultController&, JS::Value chunk);
double writable_stream_default_controller_get_desired_size(WritableStreamDefaultController const&);
void writable_stream_default_controller_process_close(WritableStreamDefaultController&);
void writable_stream_default_controller_process_write(WritableStreamDefaultController&, JS::Value chunk);
void writable_stream_default_controller_write(WritableStreamDefaultController&, JS::Value chunk, JS::Value chunk_size);

void initialize_transform_stream(TransformStream&, JS::NonnullGCPtr<JS::PromiseCapability> start_promise, double writable_high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> writable_size_algorithm, double readable_high_water_mark, JS::NonnullGCPtr<SizeAlgorithm> readable_size_algorithm);
void set_up_transform_stream_default_controller(TransformStream&, TransformStreamDefaultController&, JS::NonnullGCPtr<TransformAlgorithm>, JS::NonnullGCPtr<FlushAlgorithm>, JS::NonnullGCPtr<CancelAlgorithm>);
void set_up_transform_stream_default_controller_from_transformer(TransformStream&, JS::Value transformer, Transformer&);
void transform_stream_default_controller_clear_algorithms(TransformStreamDefaultController&);
WebIDL::ExceptionOr<void> transform_stream_default_controller_enqueue(TransformStreamDefaultController&, JS::Value chunk);
void transform_stream_default_controller_error(TransformStreamDefaultController&, JS::Value error);
void transform_stream_default_controller_terminate(TransformStreamDefaultController&);
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_controller_perform_transform(TransformStreamDefaultController&, JS::Value chunk);
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_sink_abort_algorithm(TransformStream&, JS::Value reason);
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_sink_close_algorithm(TransformStream&);
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_sink_write_algorithm(TransformStream&, JS::Value chunk);
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_source_pull_algorithm(TransformStream&);
JS::NonnullGCPtr<WebIDL::Promise> transform_stream_default_source_cancel_algorithm(TransformStream&, JS::Value reason);
void transform_stream_error(TransformStream&, JS::Value error);
void transform_stream_error_writable_and_unblock_write(TransformStream&, JS::Value error);
void transform_stream_set_backpressure(TransformStream&, bool backpressure);
void transform_stream_set_up(TransformStream&, JS::NonnullGCPtr<TransformAlgorithm>, JS::GCPtr<FlushAlgorithm> = {}, JS::GCPtr<CancelAlgorithm> = {});
void transform_stream_unblock_write(TransformStream&);

bool is_non_negative_number(JS::Value);
bool can_transfer_array_buffer(JS::ArrayBuffer const& array_buffer);
WebIDL::ExceptionOr<JS::Value> clone_as_uint8_array(JS::Realm&, WebIDL::ArrayBufferView&);
WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm&, JS::Value value);

JS::Value create_close_sentinel();
bool is_close_sentinel(JS::Value);
JS::ThrowCompletionOr<JS::Handle<WebIDL::CallbackType>> property_to_callback(JS::VM& vm, JS::Value value, JS::PropertyKey const& property_key, WebIDL::OperationReturnsPromise);

// https://streams.spec.whatwg.org/#value-with-size
struct ValueWithSize {
    JS::Value value;
    double size;
};

// https://streams.spec.whatwg.org/#dequeue-value
template<typename T>
JS::Value dequeue_value(T& container)
{
    // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.

    // 2. Assert: container.[[queue]] is not empty.
    VERIFY(!container.queue().is_empty());

    // 3. Let valueWithSize be container.[[queue]][0].
    // 4. Remove valueWithSize from container.[[queue]].
    auto value_with_size = container.queue().take_first();

    // 5. Set container.[[queueTotalSize]] to container.[[queueTotalSize]] − valueWithSize’s size.
    container.set_queue_total_size(container.queue_total_size() - value_with_size.size);

    // 6. If container.[[queueTotalSize]] < 0, set container.[[queueTotalSize]] to 0. (This can occur due to rounding errors.)
    if (container.queue_total_size() < 0.0)
        container.set_queue_total_size(0.0);

    // 7. Return valueWithSize’s value.
    return value_with_size.value;
}

// https://streams.spec.whatwg.org/#enqueue-value-with-size
template<typename T>
WebIDL::ExceptionOr<void> enqueue_value_with_size(T& container, JS::Value value, JS::Value size)
{
    // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.

    // 2. If ! IsNonNegativeNumber(size) is false, throw a RangeError exception.
    if (!is_non_negative_number(size))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Chunk has non-positive size"sv };

    // 3. If size is +∞, throw a RangeError exception.
    if (size.is_positive_infinity())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Chunk has infinite size"sv };

    // 4. Append a new value-with-size with value value and size size to container.[[queue]].
    container.queue().append({ value, size.as_double() });

    // 5. Set container.[[queueTotalSize]] to container.[[queueTotalSize]] + size.
    container.set_queue_total_size(container.queue_total_size() + size.as_double());

    return {};
}

// https://streams.spec.whatwg.org/#peek-queue-value
template<typename T>
JS::Value peek_queue_value(T& container)
{
    // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.

    // 2. Assert: container.[[queue]] is not empty.
    VERIFY(!container.queue().is_empty());

    // 3. Let valueWithSize be container.[[queue]][0].
    auto& value_with_size = container.queue().first();

    // 4. Return valueWithSize’s value.
    return value_with_size.value;
}

// https://streams.spec.whatwg.org/#reset-queue
template<typename T>
void reset_queue(T& container)
{
    // 1. Assert: container has [[queue]] and [[queueTotalSize]] internal slots.

    // 2. Set container.[[queue]] to a new empty list.
    container.queue().clear();

    // 3. Set container.[[queueTotalSize]] to 0.
    container.set_queue_total_size(0);
}

}
