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
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Streams {

// 5.5.1. Working with writable streams, https://streams.spec.whatwg.org/#ws-abstract-ops
WebIDL::ExceptionOr<GC::Ref<WritableStreamDefaultWriter>> acquire_writable_stream_default_writer(WritableStream&);
WebIDL::ExceptionOr<GC::Ref<WritableStream>> create_writable_stream(JS::Realm& realm, GC::Ref<StartAlgorithm> start_algorithm, GC::Ref<WriteAlgorithm> write_algorithm, GC::Ref<CloseAlgorithm> close_algorithm, GC::Ref<AbortAlgorithm> abort_algorithm, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm);
void initialize_writable_stream(WritableStream&);
bool is_writable_stream_locked(WritableStream const&);
WebIDL::ExceptionOr<void> set_up_writable_stream_default_writer(WritableStreamDefaultWriter&, WritableStream&);
GC::Ref<WebIDL::Promise> writable_stream_abort(WritableStream&, JS::Value reason);
GC::Ref<WebIDL::Promise> writable_stream_close(WritableStream&);

// 5.5.2. Interfacing with controllers, https://streams.spec.whatwg.org/#ws-abstract-ops-used-by-controllers
GC::Ref<WebIDL::Promise> writable_stream_add_write_request(WritableStream&);
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

// 5.5.3. Writers, https://streams.spec.whatwg.org/#ws-writer-abstract-ops
GC::Ref<WebIDL::Promise> writable_stream_default_writer_abort(WritableStreamDefaultWriter&, JS::Value reason);
GC::Ref<WebIDL::Promise> writable_stream_default_writer_close(WritableStreamDefaultWriter&);
GC::Ref<WebIDL::Promise> writable_stream_default_writer_close_with_error_propagation(WritableStreamDefaultWriter&);
void writable_stream_default_writer_ensure_closed_promise_rejected(WritableStreamDefaultWriter&, JS::Value error);
void writable_stream_default_writer_ensure_ready_promise_rejected(WritableStreamDefaultWriter&, JS::Value error);
Optional<double> writable_stream_default_writer_get_desired_size(WritableStreamDefaultWriter const&);
void writable_stream_default_writer_release(WritableStreamDefaultWriter&);
GC::Ref<WebIDL::Promise> writable_stream_default_writer_write(WritableStreamDefaultWriter&, JS::Value chunk);

// 5.5.4. Default controllers, https://streams.spec.whatwg.org/#ws-default-controller-abstract-ops
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller(WritableStream&, WritableStreamDefaultController&, GC::Ref<StartAlgorithm>, GC::Ref<WriteAlgorithm>, GC::Ref<CloseAlgorithm>, GC::Ref<AbortAlgorithm>, double high_water_mark, GC::Ref<SizeAlgorithm>);
WebIDL::ExceptionOr<void> set_up_writable_stream_default_controller_from_underlying_sink(WritableStream&, JS::Value underlying_sink_value, UnderlyingSink&, double high_water_mark, GC::Ref<SizeAlgorithm> size_algorithm);
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

}
