/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/WholeBodyRead.h>

using namespace Web::Fetch::Engine;
using Web::Fetch::Infrastructure::ResponseBody;

namespace {

// The credit the channel grants its transport.
class CreditRecordingProducerSink final : public FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64 byte_count) override { granted += byte_count; }
    virtual void consumer_cancelled() override { }

    u64 granted { 0 };
};

// Writes to its channel at the first credit the channel grants it, and closes the channel at the second.
class WritingOnCreditProducerSink final : public FetchByteChannelProducerSink {
public:
    virtual void grant_credit(u64) override
    {
        if (!channel)
            return;
        if (++grant_count == 1) {
            EXPECT_EQ(channel->write(MUST(Core::ImmutableBytes::copy(" and the rest"sv.bytes()))), FetchByteChannel::WriteResult::Accepted);
            return;
        }
        EXPECT_EQ(channel->close(), FetchByteChannel::TerminateResult::Terminated);
        channel = nullptr;
    }
    virtual void consumer_cancelled() override { }

    FetchByteChannel* channel { nullptr };
    size_t grant_count { 0 };
};

// The results the steps of a read ran with.
struct ReadResults : RefCounted<ReadResults> {
    Vector<WholeBodyReadResult> results;
};

}

static NonnullRefPtr<ReadResults> read(ResponseBody const& body)
{
    auto results = make_ref_counted<ReadResults>();
    read_whole_body(body, [results](WholeBodyReadResult result) { results->results.append(move(result)); });
    return results;
}

static NonnullRefPtr<FetchByteChannel> create_channel(RefPtr<FetchByteChannelProducerSink> producer = {})
{
    if (!producer)
        producer = adopt_ref(*new CreditRecordingProducerSink);
    return FetchByteChannel::create({ .low = 16, .high = 64 }, producer.release_nonnull());
}

static Core::ImmutableBytes bytes_of(StringView string)
{
    return MUST(Core::ImmutableBytes::copy(string.bytes()));
}

static bool read_bytes(ReadResults const& read, StringView expected)
{
    if (read.results.size() != 1)
        return false;
    auto const* bytes = read.results[0].get_pointer<ByteBuffer>();
    return bytes && bytes->bytes() == expected.bytes();
}

static bool failed(ReadResults const& read)
{
    return read.results.size() == 1 && read.results[0].has<WholeBodyReadFailure>();
}

TEST_CASE(a_body_of_bytes_is_read_at_once)
{
    auto read_body = read(ResponseBody::from_bytes(bytes_of("whole body"sv)));
    EXPECT(read_bytes(read_body, "whole body"sv));
}

TEST_CASE(a_channel_filled_in_parts_is_read_once_it_closes)
{
    auto channel = create_channel();
    EXPECT_EQ(channel->write(bytes_of("first "sv)), FetchByteChannel::WriteResult::Accepted);
    auto read_body = read(ResponseBody { channel, {} });
    EXPECT_EQ(channel->write(bytes_of("second "sv)), FetchByteChannel::WriteResult::Accepted);
    EXPECT_EQ(channel->write_copy("third"sv.bytes()), FetchByteChannel::WriteResult::Accepted);
    EXPECT(read_body->results.is_empty());

    EXPECT_EQ(channel->close(), FetchByteChannel::TerminateResult::Terminated);
    EXPECT(read_bytes(read_body, "first second third"sv));
}

TEST_CASE(a_channel_closed_before_the_read_is_read_at_once)
{
    auto channel = create_channel();
    EXPECT_EQ(channel->write(bytes_of("whole body"sv)), FetchByteChannel::WriteResult::Accepted);
    EXPECT_EQ(channel->close(), FetchByteChannel::TerminateResult::Terminated);
    EXPECT(read_bytes(read(ResponseBody { channel, {} }), "whole body"sv));
}

// Each write and the close come while the read takes bytes, and wake it again as it waits for more.
TEST_CASE(a_channel_written_and_closed_as_the_read_grants_credit_is_read_to_its_end)
{
    auto producer = adopt_ref(*new WritingOnCreditProducerSink);
    auto channel = create_channel(producer);
    producer->channel = channel.ptr();
    EXPECT_EQ(channel->write(bytes_of("the start"sv)), FetchByteChannel::WriteResult::Accepted);

    auto read_body = read(ResponseBody { channel, {} });
    EXPECT_EQ(producer->grant_count, 2u);
    EXPECT(read_bytes(read_body, "the start and the rest"sv));
}

TEST_CASE(a_channel_that_errors_fails_the_read)
{
    auto channel = create_channel();
    EXPECT_EQ(channel->write(bytes_of("the start"sv)), FetchByteChannel::WriteResult::Accepted);
    auto read_body = read(ResponseBody { channel, {} });
    EXPECT_EQ(channel->error({ .message = MUST(ByteBuffer::copy("failed"sv.bytes())) }), FetchByteChannel::TerminateResult::Terminated);
    EXPECT(failed(read_body));
}

TEST_CASE(a_body_larger_than_the_high_watermark_is_read_as_its_reads_grant_credit)
{
    auto producer = adopt_ref(*new CreditRecordingProducerSink);
    auto channel = create_channel(producer);
    auto read_body = read(ResponseBody { channel, {} });

    // The transport delivers what it is granted, a part at a time, and the read takes each part.
    auto const body_size = 1000u;
    u64 allowed = channel->initial_credit();
    StringBuilder body;
    while (body.length() < body_size) {
        auto part_size = min<u64>(min<u64>(allowed - body.length(), 16), body_size - body.length());
        EXPECT(part_size > 0);
        if (part_size == 0)
            break;
        auto part = ByteString::repeated(static_cast<char>('a' + body.length() % 26), part_size);
        EXPECT_EQ(channel->write(bytes_of(part)), FetchByteChannel::WriteResult::Accepted);
        body.append(part);
        allowed = channel->initial_credit() + producer->granted;
    }
    EXPECT(producer->granted > 0);
    EXPECT(read_body->results.is_empty());

    EXPECT_EQ(channel->close(), FetchByteChannel::TerminateResult::Terminated);
    EXPECT(read_bytes(read_body, body.string_view()));
}

TEST_CASE(a_cancelled_channel_fails_the_read)
{
    auto cancelled_during_the_read = create_channel();
    EXPECT_EQ(cancelled_during_the_read->write(bytes_of("the start"sv)), FetchByteChannel::WriteResult::Accepted);
    auto read_body = read(ResponseBody { cancelled_during_the_read, {} });
    EXPECT_EQ(cancelled_during_the_read->cancel(), FetchByteChannel::TerminateResult::Terminated);
    EXPECT(failed(read_body));

    auto cancelled_before_the_read = create_channel();
    EXPECT_EQ(cancelled_before_the_read->cancel(), FetchByteChannel::TerminateResult::Terminated);
    EXPECT(failed(read(ResponseBody { cancelled_before_the_read, {} })));
}

TEST_CASE(a_transferred_channel_fails_the_read)
{
    auto channel = create_channel();
    auto read_body = read(ResponseBody { channel, {} });
    (void)channel->transfer_out();
    EXPECT(failed(read_body));
}
