/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibTest/TestCase.h>
#include <LibThreading/Thread.h>

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
    stream->reached_end_of_body();

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
    Core::EventLoop loop;

    auto stream = Media::IncrementallyPopulatedStream::create_empty();
    stream->set_expected_size(100);
    stream->set_data_request_callback([](u64) { });

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

    EXPECT(!cursor->is_blocked());

    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> read_completed { false };
    IGNORE_USE_IN_ESCAPING_LAMBDA Atomic<bool> was_aborted { false };

    auto thread = Threading::Thread::construct("TestAbort"sv, [&, cursor]() -> intptr_t {
        Array<u8, 10> buffer;
        auto result = cursor->read_into(buffer);
        read_completed = true;
        was_aborted = result.is_error() && result.error().category() == Media::DecoderErrorCategory::Aborted;
        return 0;
    });

    thread->start();

    while (!cursor->is_blocked())
        ;
    EXPECT(cursor->is_blocked());

    cursor->abort();
    MUST(thread->join());

    EXPECT_EQ(cursor->is_blocked(), false);
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

TEST_CASE(data_request_callback_invoked)
{
    Core::EventLoop loop;

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

    stream->set_data_request_callback([&](u64 offset) {
        auto data = make_test_data(100);
        stream->add_chunk_at(seek_position, data.bytes());
        callback_invoked = true;
        requested_offset = offset;
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
