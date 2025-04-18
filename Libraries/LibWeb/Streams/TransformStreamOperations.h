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

// 6.4.1. Working with transform streams, https://streams.spec.whatwg.org/#ts-abstract-ops
void initialize_transform_stream(TransformStream&, GC::Ref<WebIDL::Promise> start_promise, double writable_high_water_mark, GC::Ref<SizeAlgorithm> writable_size_algorithm, double readable_high_water_mark, GC::Ref<SizeAlgorithm> readable_size_algorithm);
void transform_stream_error(TransformStream&, JS::Value error);
void transform_stream_error_writable_and_unblock_write(TransformStream&, JS::Value error);
void transform_stream_set_backpressure(TransformStream&, bool backpressure);
void transform_stream_unblock_write(TransformStream&);

// 6.4.2. Default controllers, https://streams.spec.whatwg.org/#ts-default-controller-abstract-ops
void set_up_transform_stream_default_controller(TransformStream&, TransformStreamDefaultController&, GC::Ref<TransformAlgorithm>, GC::Ref<FlushAlgorithm>, GC::Ref<CancelAlgorithm>);
void set_up_transform_stream_default_controller_from_transformer(TransformStream&, JS::Value transformer, Transformer&);
void transform_stream_default_controller_clear_algorithms(TransformStreamDefaultController&);
WebIDL::ExceptionOr<void> transform_stream_default_controller_enqueue(TransformStreamDefaultController&, JS::Value chunk);
void transform_stream_default_controller_error(TransformStreamDefaultController&, JS::Value error);
GC::Ref<WebIDL::Promise> transform_stream_default_controller_perform_transform(TransformStreamDefaultController&, JS::Value chunk);
void transform_stream_default_controller_terminate(TransformStreamDefaultController&);

// 6.4.3. Default sinks, https://streams.spec.whatwg.org/#ts-default-sink-abstract-ops
GC::Ref<WebIDL::Promise> transform_stream_default_sink_write_algorithm(TransformStream&, JS::Value chunk);
GC::Ref<WebIDL::Promise> transform_stream_default_sink_abort_algorithm(TransformStream&, JS::Value reason);
GC::Ref<WebIDL::Promise> transform_stream_default_sink_close_algorithm(TransformStream&);

// 6.4.4. Default sources, https://streams.spec.whatwg.org/#ts-default-source-abstract-ops
GC::Ref<WebIDL::Promise> transform_stream_default_source_cancel_algorithm(TransformStream&, JS::Value reason);
GC::Ref<WebIDL::Promise> transform_stream_default_source_pull_algorithm(TransformStream&);

}
