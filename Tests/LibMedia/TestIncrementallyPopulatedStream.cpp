/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

#include "TestMediaCommon.h"

static ByteBuffer make_test_data(size_t size)
{
    auto buffer = MUST(ByteBuffer::create_uninitialized(size));
    for (size_t i = 0; i < size; i++)
        buffer[i] = static_cast<u8>(i);
    return buffer;
}

TEST_CASE(create_empty)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    EXPECT(!stream->expected_size().has_value());

    stream->set_expected_size(500);
    EXPECT(stream->expected_size().has_value());
    EXPECT_EQ(stream->expected_size().value(), 500u);
}

TEST_CASE(create_from_data_and_buffer)
{
    auto data = make_test_data(256);

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(data);
    EXPECT(stream->expected_size().has_value());
    EXPECT_EQ(stream->expected_size().value(), 256u);
    EXPECT_EQ(stream->size(), 256u);
}

TEST_CASE(cursor_seek_modes)
{
    auto data = make_test_data(100);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(data.bytes());
    auto cursor = stream->create_cursor();

    EXPECT_EQ(cursor->position(), 0u);
    EXPECT_EQ(cursor->size(), 100u);

    MUST(cursor->seek(50, SeekMode::SetPosition));
    EXPECT_EQ(cursor->position(), 50u);

    MUST(cursor->seek(10, SeekMode::FromCurrentPosition));
    EXPECT_EQ(cursor->position(), 60u);

    MUST(cursor->seek(-10, SeekMode::FromEndPosition));
    EXPECT_EQ(cursor->position(), 90u);
}

TEST_CASE(cursor_read_operations)
{
    auto data = make_test_data(100);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(data.bytes());
    auto cursor = stream->create_cursor();

    Array<u8, 10> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 10u);
    EXPECT_EQ(cursor->position(), 10u);
    for (size_t i = 0; i < 10; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));

    MUST(cursor->seek(50, SeekMode::SetPosition));
    MUST(cursor->read_into(buffer));
    for (size_t i = 0; i < 10; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(50 + i));

    MUST(cursor->seek(95, SeekMode::SetPosition));
    bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 5u);
    for (size_t i = 0; i < 5; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(95 + i));

    MUST(cursor->seek(100, SeekMode::SetPosition));
    auto result = cursor->read_into(buffer);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::EndOfStream);

    MUST(cursor->seek(0, SeekMode::SetPosition));
    bytes_read = MUST(cursor->read_into(buffer.span().trim(0)));
    EXPECT_EQ(bytes_read, 0u);
    EXPECT_EQ(cursor->position(), 0u);

    EXPECT_EQ(MUST(cursor->read_value<u32>()), 0x00010203u);
}

TEST_CASE(sequential_reads)
{
    auto data = make_test_data(256);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(data.bytes());
    auto cursor = stream->create_cursor();

    for (size_t i = 0; i < 256; i += 16) {
        Array<u8, 16> buffer;
        auto bytes_read = MUST(cursor->read_into(buffer));
        EXPECT_EQ(bytes_read, 16u);

        for (size_t j = 0; j < 16; j++)
            EXPECT_EQ(buffer[j], static_cast<u8>(i + j));
    }

    EXPECT_EQ(cursor->position(), 256u);
}

TEST_CASE(multiple_cursors_independent)
{
    auto data = make_test_data(100);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(data.bytes());
    auto cursor1 = stream->create_cursor();
    auto cursor2 = stream->create_cursor();

    MUST(cursor1->seek(10, SeekMode::SetPosition));
    MUST(cursor2->seek(50, SeekMode::SetPosition));

    EXPECT_EQ(cursor1->position(), 10u);
    EXPECT_EQ(cursor2->position(), 50u);

    Array<u8, 5> buffer1;
    Array<u8, 5> buffer2;
    MUST(cursor1->read_into(buffer1));
    MUST(cursor2->read_into(buffer2));

    for (size_t i = 0; i < 5; i++) {
        EXPECT_EQ(buffer1[i], static_cast<u8>(10 + i));
        EXPECT_EQ(buffer2[i], static_cast<u8>(50 + i));
    }
}

TEST_CASE(add_chunks_incrementally)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();

    constexpr size_t data_size = 100;
    auto data = make_test_data(data_size);
    stream->add_chunk_at(0, data.bytes().trim(50));

    stream->add_chunk_at(50, data.bytes().slice(50));
    stream->close();

    EXPECT(stream->expected_size().has_value());
    EXPECT_EQ(stream->expected_size().value(), data_size);

    auto cursor = stream->create_cursor();
    Array<u8, data_size> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));

    EXPECT_EQ(bytes_read, data_size);
    for (size_t i = 0; i < data_size; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
}

TEST_CASE(add_overlapping_chunks)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();

    constexpr size_t data_size = 100;
    auto data = make_test_data(data_size);
    stream->add_chunk_at(0, data.bytes().trim(50));
    stream->add_chunk_at(40, data.bytes().slice(40));

    auto cursor = stream->create_cursor();
    Array<u8, data_size> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));

    EXPECT_EQ(bytes_read, data_size);
    for (size_t i = 0; i < data_size; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
}

TEST_CASE(add_chunk_at_offset)
{
    never_destroyed_event_loop();

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);
    stream->set_data_request_callback([](Optional<u64>) { });

    auto data = make_test_data(80);
    stream->add_chunk_at(0, data.bytes().trim(30));
    stream->add_chunk_at(50, data.bytes().slice(50));

    auto cursor = stream->create_cursor();
    MUST(cursor->seek(50, SeekMode::SetPosition));

    Array<u8, 30> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));

    EXPECT_EQ(bytes_read, 30u);
    for (size_t i = 0; i < 30; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(50 + i));
}

TEST_CASE(cursor_abort_and_reset)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);

    auto cursor = stream->create_cursor();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_completed { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> was_aborted { false };

    cursor->set_blocked_change_handler([&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });

    EXPECT(!read_blocked.load());

    auto thread = Threading::Thread::construct("TestAbort"sv, [&, cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        auto result = cursor->read_into(buffer);
        read_completed = true;
        was_aborted = result.is_error() && result.error().category() == Media::DecoderErrorCategory::Aborted;
        return 0;
    });

    thread->start();

    while (!read_blocked.load())
        ;
    EXPECT(read_blocked.load());

    cursor->abort();
    MUST(thread->join());

    EXPECT(!read_blocked.load());
    EXPECT(read_completed.load());
    EXPECT(was_aborted.load());

    // After aborting a read, reset_abort() should allow us to read again.
    cursor->reset_abort();
    auto data = make_test_data(100);
    stream->add_chunk_at(0, data.bytes());

    Array<u8, 10> buffer;
    auto result = cursor->read_into(buffer);
    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), 10u);
}

TEST_CASE(cursor_blocked_change_handler)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);

    auto cursor = stream->create_cursor();

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> blocked_count { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<u32> unblocked_count { 0 };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> last_state { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_succeeded { false };

    cursor->set_blocked_change_handler([&](Media::ReadBlocked blocked) {
        if (blocked == Media::ReadBlocked::Yes)
            blocked_count++;
        else
            unblocked_count++;
        last_state = blocked == Media::ReadBlocked::Yes;
    });

    auto thread = Threading::Thread::construct("TestBlockHandler"sv, [&, cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        read_succeeded = !cursor->read_into(buffer).is_error();
        return 0;
    });
    thread->start();

    // The read finds no data and parks, firing the handler with blocked=true exactly once.
    while (blocked_count.load() == 0)
        ;
    EXPECT_EQ(blocked_count.load(), 1u);
    EXPECT_EQ(unblocked_count.load(), 0u);
    EXPECT(last_state.load());

    // Appending the awaited data resumes the read, firing the handler with blocked=false.
    auto data = make_test_data(100);
    stream->add_chunk_at(0, data.bytes());

    MUST(thread->join());
    EXPECT(read_succeeded.load());
    EXPECT_EQ(blocked_count.load(), 1u);
    EXPECT_EQ(unblocked_count.load(), 1u);
    EXPECT(!last_state.load());
}

TEST_CASE(redundant_chunk_within_existing_chunk_at_nonzero_offset)
{
    // Regression test: add_chunk_at used to compare chunk.size() (a relative byte count)
    // against new_chunk_end (an absolute file offset). When the existing chunk started at
    // a non-zero offset, chunk.size() < new_chunk_end even if the new data was fully
    // covered, causing the buffer to be shrunk and data beyond the new chunk's end to be lost.
    auto stream = Media::IncrementallyPopulatedStream::create_empty();

    constexpr size_t data_size = 200;
    auto data = make_test_data(data_size);

    // Add a chunk at a non-zero offset covering [100, 120).
    stream->add_chunk_at(100, data.bytes().slice(100, 20));

    // Add a redundant chunk fully within [100, 120), specifically [105, 115).
    // With the bug, this shrinks the existing chunk to [100, 115), losing bytes [115, 120).
    stream->add_chunk_at(105, data.bytes().slice(105, 10));

    stream->close();

    auto cursor = stream->create_cursor();
    MUST(cursor->seek(100, SeekMode::SetPosition));

    Array<u8, 20> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));

    EXPECT_EQ(bytes_read, 20u);
    for (size_t i = 0; i < 20; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(100 + i));
}

TEST_CASE(remove_byte_range_splits_chunk)
{
    auto data = make_test_data(100);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(data.bytes());

    stream->remove_byte_range(30, 70);

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 30u);
    EXPECT_EQ(ranges[1].start, 70u);
    EXPECT_EQ(ranges[1].end, 100u);

    auto cursor = stream->create_cursor();
    Array<u8, 30> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 30u);
    for (size_t i = 0; i < 30; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));

    MUST(cursor->seek(70, SeekMode::SetPosition));
    bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 30u);
    for (size_t i = 0; i < 30; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(70 + i));
}

TEST_CASE(remove_byte_range_trims_chunk)
{
    auto data = make_test_data(100);
    auto stream = Media::IncrementallyPopulatedStream::create_from_data(data.bytes());

    stream->remove_byte_range(0, 20);
    stream->remove_byte_range(80, 100);

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 20u);
    EXPECT_EQ(ranges[0].end, 80u);

    auto cursor = stream->create_cursor();
    MUST(cursor->seek(20, SeekMode::SetPosition));

    Array<u8, 60> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 60u);
    for (size_t i = 0; i < 60; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(20 + i));
}

TEST_CASE(remove_byte_range_across_disjoint_chunks)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(0, data.bytes().trim(20));
    stream->add_chunk_at(40, data.bytes().slice(40, 20));
    stream->add_chunk_at(80, data.bytes().slice(80));
    stream->remove_byte_range(10, 90);

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 10u);
    EXPECT_EQ(ranges[1].start, 90u);
    EXPECT_EQ(ranges[1].end, 100u);

    auto cursor = stream->create_cursor();
    Array<u8, 10> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 10u);
    for (size_t i = 0; i < 10; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));

    MUST(cursor->seek(90, SeekMode::SetPosition));
    bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 10u);
    for (size_t i = 0; i < 10; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(90 + i));
}

TEST_CASE(add_touching_chunks_forward)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(0, data.bytes().trim(50));
    stream->add_chunk_at(50, data.bytes().slice(50));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 100u);
}

TEST_CASE(add_touching_chunks_reverse)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(50, data.bytes().slice(50));
    stream->add_chunk_at(0, data.bytes().trim(50));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 100u);

    // Verify the data is contiguous and correct.
    auto cursor = stream->create_cursor();
    Array<u8, 100> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 100u);
    for (size_t i = 0; i < 100; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
}

TEST_CASE(add_disjoint_chunks)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(0, data.bytes().trim(30));
    stream->add_chunk_at(70, data.bytes().slice(70));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 30u);
    EXPECT_EQ(ranges[1].start, 70u);
    EXPECT_EQ(ranges[1].end, 100u);
}

TEST_CASE(add_chunk_fills_gap)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(0, data.bytes().trim(30));
    stream->add_chunk_at(70, data.bytes().slice(70));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 2u);

    // Fill the gap.
    stream->add_chunk_at(30, data.bytes().slice(30, 40));

    ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 100u);

    // Verify data integrity.
    auto cursor = stream->create_cursor();
    Array<u8, 100> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 100u);
    for (size_t i = 0; i < 100; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
}

TEST_CASE(add_chunk_spans_multiple_existing_chunks)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(120);

    stream->add_chunk_at(0, data.bytes().trim(10));
    stream->add_chunk_at(20, data.bytes().slice(20, 10));
    stream->add_chunk_at(40, data.bytes().slice(40, 10));
    stream->add_chunk_at(10, data.bytes().slice(10, 60));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 70u);

    auto cursor = stream->create_cursor();
    Array<u8, 70> buffer;
    auto bytes_read = MUST(cursor->read_into(buffer));
    EXPECT_EQ(bytes_read, 70u);
    for (size_t i = 0; i < 70; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
}

TEST_CASE(add_three_disjoint_then_connect)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(150);

    stream->add_chunk_at(0, data.bytes().trim(30));
    stream->add_chunk_at(60, data.bytes().slice(60, 30));
    stream->add_chunk_at(120, data.bytes().slice(120));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 3u);

    // Connect first and second with a chunk that touches both.
    stream->add_chunk_at(30, data.bytes().slice(30, 30));
    ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 2u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 90u);

    // Connect second and third.
    stream->add_chunk_at(90, data.bytes().slice(90, 30));
    ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 150u);
}

TEST_CASE(data_request_callback_invoked)
{
    auto& loop = never_destroyed_event_loop();

    // Stream size must be larger than FORWARD_REQUEST_THRESHOLD (1 MiB) to test callback
    static constexpr u64 stream_size = 2 * MiB;
    static constexpr u64 initial_chunk_size = 100;
    static constexpr u64 seek_position = stream_size - 100;

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(stream_size);

    // Add initial chunk so the callback logic can be triggered
    auto initial_data = make_test_data(initial_chunk_size);
    stream->add_chunk_at(0, initial_data.bytes());

    bool callback_invoked { false };
    u64 requested_offset { 0 };

    stream->set_data_request_callback([&](Optional<u64> offset) {
        auto data = make_test_data(100);
        stream->add_chunk_at(seek_position, data.bytes());
        callback_invoked = true;
        requested_offset = offset.value();
    });

    auto cursor = stream->create_cursor();
    MUST(cursor->seek(seek_position, SeekMode::SetPosition));

    auto thread = Threading::Thread::construct("TestCallback"sv, [cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        MUST(cursor->read_into(buffer));
        return 0;
    });
    thread->start();

    auto start_time = MonotonicTime::now_coarse();
    while (!callback_invoked) {
        loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        if (MonotonicTime::now_coarse() - start_time > AK::Duration::from_seconds(1))
            break;
    }

    EXPECT(callback_invoked);
    EXPECT(requested_offset >= initial_chunk_size);

    MUST(thread->join());
}

TEST_CASE(close_crossing_a_new_request_keeps_the_end_of_the_closing_fetch)
{
    auto& loop = never_destroyed_event_loop();

    static constexpr u64 stream_size = 2 * MiB;
    static constexpr u64 tail_position = stream_size - 100;
    auto data = make_test_data(stream_size);

    IGNORE_USE_IN_ESCAPING_LAMBDA auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(stream_size);
    stream->add_chunk_at(0, data.bytes().trim(100));

    IGNORE_USE_IN_ESCAPING_LAMBDA Vector<u64> requested_offsets;
    stream->set_data_request_callback([&](Optional<u64> offset) {
        if (!offset.has_value())
            return;
        requested_offsets.append(*offset);
        if (requested_offsets.size() == 1) {
            // The fetch from the requested offset runs to the end of the resource.
            stream->add_chunk_at(*offset, data.bytes().slice(*offset));
            return;
        }
        // The first fetch's close arrives after the stream has already requested data elsewhere.
        stream->close();
        stream->add_chunk_at(*offset, data.bytes().slice(*offset, 200));
    });

    auto read_on_thread = [&](u64 position) {
        IGNORE_USE_IN_ESCAPING_LAMBDA auto cursor = stream->create_cursor();
        MUST(cursor->seek(position, SeekMode::SetPosition));
        IGNORE_USE_IN_ESCAPING_LAMBDA Optional<Media::DecoderErrorOr<size_t>> result;
        IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> finished { false };
        auto thread = Threading::Thread::construct("TestRead"sv, [&]() -> intptr_t {
            Array<u8, 10> buffer;
            result = cursor->read_into(buffer);
            finished = true;
            return 0;
        });
        thread->start();
        auto start_time = MonotonicTime::now_coarse();
        while (!finished && MonotonicTime::now_coarse() - start_time < AK::Duration::from_seconds(1))
            loop.pump(Core::EventLoop::WaitMode::PollForEvents);
        if (!finished)
            cursor->abort();
        MUST(thread->join());
        return result.release_value();
    };

    EXPECT_EQ(MUST(read_on_thread(tail_position)), 10u);
    EXPECT_EQ(requested_offsets.size(), 1u);

    auto result = read_on_thread(100);
    EXPECT_EQ(requested_offsets.size(), 2u);
    EXPECT(!result.is_error());
    EXPECT_EQ(stream->expected_size().value(), stream_size);
}

TEST_CASE(chunks_past_the_end_are_dropped_once_closed)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(0, data.bytes());
    stream->close();
    EXPECT_EQ(stream->expected_size().value(), 100u);

    auto extra_data = make_test_data(50);
    stream->add_chunk_at(100, extra_data.bytes());

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 100u);

    // A chunk straddling the end is cut at the end.
    stream->add_chunk_at(90, extra_data.bytes().trim(20));

    ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].end, 100u);

    auto cursor = stream->create_cursor();
    MUST(cursor->seek(90, SeekMode::SetPosition));

    Array<u8, 10> buffer;
    EXPECT_EQ(MUST(cursor->read_into(buffer)), 10u);
    for (size_t i = 0; i < 10; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(90 + i));

    auto result = cursor->read_into(buffer);
    EXPECT(result.is_error());
    EXPECT_EQ(result.error().category(), Media::DecoderErrorCategory::EndOfStream);
}

TEST_CASE(chunks_before_the_end_are_accepted_once_closed)
{
    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    auto data = make_test_data(100);

    stream->add_chunk_at(60, data.bytes().slice(60));
    stream->close();
    EXPECT_EQ(stream->expected_size().value(), 100u);

    stream->add_chunk_at(0, data.bytes().trim(60));

    auto ranges = stream->available_byte_ranges();
    EXPECT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].start, 0u);
    EXPECT_EQ(ranges[0].end, 100u);
    EXPECT_EQ(stream->expected_size().value(), 100u);

    auto cursor = stream->create_cursor();
    Array<u8, 100> buffer;
    EXPECT_EQ(MUST(cursor->read_into(buffer)), 100u);
    for (size_t i = 0; i < 100; i++)
        EXPECT_EQ(buffer[i], static_cast<u8>(i));
}

TEST_CASE(idling_stops_the_request_once_no_read_is_waiting)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);

    Vector<Optional<u64>> requests;
    stream->set_data_request_callback([&](Optional<u64> offset) {
        requests.append(offset);
    });

    auto cursor = stream->create_cursor();
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    cursor->set_blocked_change_handler([&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });
    auto thread = Threading::Thread::construct("TestIdle"sv, [cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        MUST(cursor->read_into(buffer));
        return 0;
    });
    thread->start();
    while (!read_blocked.load())
        ;

    // The initial fetch keeps going while the read waits on it.
    stream->set_may_idle(true);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT(requests.is_empty());

    auto data = make_test_data(100);
    stream->add_chunk_at(0, data.bytes());
    MUST(thread->join());
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 1u);
    EXPECT(!requests[0].has_value());

    // Nothing is waiting, so withdrawing the permission asks for nothing.
    stream->set_may_idle(false);
    stream->set_may_idle(true);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 1u);
}

TEST_CASE(withdrawing_idle_asks_for_the_data_a_read_is_waiting_on)
{
    auto& loop = never_destroyed_event_loop();

    static constexpr u64 stream_size = 2 * MiB;
    static constexpr u64 seek_position = stream_size - 100;

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(stream_size);
    auto initial_data = make_test_data(100);
    stream->add_chunk_at(0, initial_data.bytes());

    Vector<Optional<u64>> requests;
    stream->set_data_request_callback([&](Optional<u64> offset) {
        requests.append(offset);
    });

    stream->set_may_idle(true);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 1u);
    EXPECT(!requests[0].has_value());

    auto cursor = stream->create_cursor();
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    cursor->set_blocked_change_handler([&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });
    MUST(cursor->seek(seek_position, SeekMode::SetPosition));
    auto thread = Threading::Thread::construct("TestWithdraw"sv, [cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        MUST(cursor->read_into(buffer));
        return 0;
    });
    thread->start();
    while (!read_blocked.load())
        ;

    // A read that starts waiting while idle is allowed does not ask for data.
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 1u);

    stream->set_may_idle(false);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 2u);
    EXPECT(requests[1].has_value());
    EXPECT(requests[1].value() >= 100u);
    EXPECT(requests[1].value() <= seek_position);

    auto data = make_test_data(100);
    stream->add_chunk_at(seek_position, data.bytes());
    MUST(thread->join());
}

TEST_CASE(withdrawing_idle_asks_for_data_before_any_arrived)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);

    Vector<Optional<u64>> requests;
    stream->set_data_request_callback([&](Optional<u64> offset) {
        requests.append(offset);
    });

    stream->set_may_idle(true);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 1u);
    EXPECT(!requests[0].has_value());

    auto cursor = stream->create_cursor();
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    cursor->set_blocked_change_handler([&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });
    auto thread = Threading::Thread::construct("TestWithdrawEmpty"sv, [cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        MUST(cursor->read_into(buffer));
        return 0;
    });
    thread->start();
    while (!read_blocked.load())
        ;

    stream->set_may_idle(false);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1], 0u);

    auto data = make_test_data(100);
    stream->add_chunk_at(0, data.bytes());
    MUST(thread->join());
}

TEST_CASE(a_read_after_idle_is_withdrawn_asks_for_data_when_none_has_arrived)
{
    auto& loop = never_destroyed_event_loop();

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);

    Vector<Optional<u64>> requests;
    stream->set_data_request_callback([&](Optional<u64> offset) {
        requests.append(offset);
    });

    stream->set_may_idle(true);
    stream->set_may_idle(false);
    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 1u);
    EXPECT(!requests[0].has_value());

    auto cursor = stream->create_cursor();
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_blocked { false };
    cursor->set_blocked_change_handler([&](Media::ReadBlocked blocked) {
        read_blocked = blocked == Media::ReadBlocked::Yes;
    });
    auto thread = Threading::Thread::construct("TestReadAfterWithdraw"sv, [cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        MUST(cursor->read_into(buffer));
        return 0;
    });
    thread->start();
    while (!read_blocked.load())
        ;

    loop.pump(Core::EventLoop::WaitMode::PollForEvents);
    EXPECT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1], 0u);

    auto data = make_test_data(100);
    stream->add_chunk_at(0, data.bytes());
    MUST(thread->join());
}
