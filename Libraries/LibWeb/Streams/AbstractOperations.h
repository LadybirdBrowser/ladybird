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

// 7.4. Abstract operations, https://streams.spec.whatwg.org/#qs-abstract-ops
WebIDL::ExceptionOr<double> extract_high_water_mark(QueuingStrategy const&, double default_hwm);
GC::Ref<SizeAlgorithm> extract_size_algorithm(JS::VM&, QueuingStrategy const&);

// 8.2. Transferable streams, https://streams.spec.whatwg.org/#transferrable-streams
void cross_realm_transform_send_error(JS::Realm&, HTML::MessagePort&, JS::Value error);
WebIDL::ExceptionOr<void> pack_and_post_message(JS::Realm&, HTML::MessagePort&, StringView type, JS::Value value);
WebIDL::ExceptionOr<void> pack_and_post_message_handling_error(JS::Realm&, HTML::MessagePort&, StringView type, JS::Value value);
void set_up_cross_realm_transform_readable(JS::Realm&, ReadableStream&, HTML::MessagePort&);
void set_up_cross_realm_transform_writable(JS::Realm&, WritableStream&, HTML::MessagePort&);

// 8.3. Miscellaneous, https://streams.spec.whatwg.org/#misc-abstract-ops
bool can_transfer_array_buffer(JS::ArrayBuffer const& array_buffer);
bool is_non_negative_number(JS::Value);
WebIDL::ExceptionOr<GC::Ref<JS::ArrayBuffer>> transfer_array_buffer(JS::Realm& realm, JS::ArrayBuffer& buffer);
WebIDL::ExceptionOr<JS::Value> clone_as_uint8_array(JS::Realm&, WebIDL::ArrayBufferView&);
WebIDL::ExceptionOr<JS::Value> structured_clone(JS::Realm&, JS::Value value);
bool can_copy_data_block_bytes_buffer(JS::ArrayBuffer const& to_buffer, u64 to_index, JS::ArrayBuffer const& from_buffer, u64 from_index, u64 count);

// 8.1. Queue-with-sizes, https://streams.spec.whatwg.org/#queue-with-sizes

// https://streams.spec.whatwg.org/#value-with-size
struct ValueWithSize {
    JS::Value value;
    double size { 0 };
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
