/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TemporaryChange.h>
#include <LibWeb/Fetch/Engine/FetchByteChannel.h>
#include <LibWeb/Fetch/Engine/WholeBodyRead.h>

namespace Web::Fetch::Engine {

namespace {

// Takes a channel's bytes as they arrive, until the channel ends.
class ChannelRead final : public FetchByteChannelConsumerSink {
public:
    ChannelRead(NonnullRefPtr<FetchByteChannel> channel, Function<void(WholeBodyReadResult)> steps)
        : m_channel(move(channel))
        , m_steps(move(steps))
    {
    }

    void start()
    {
        m_channel->attach_consumer(*this);
        wake({});
    }

    virtual void wake(WakeTicket) override
    {
        NonnullRefPtr protector = *this;

        // An arm_waiter() call can wake the read again before it returns.
        m_woken = true;
        if (m_reading)
            return;
        TemporaryChange reading { m_reading, true };
        while (m_channel && m_woken) {
            m_woken = false;
            take_what_has_arrived();
        }
    }

private:
    void take_what_has_arrived()
    {
        auto taken = m_channel->take_all();
        for (auto const& chunk : taken.taken) {
            if (m_bytes.try_append(chunk.bytes()).is_error()) {
                m_channel->cancel();
                finish(WholeBodyReadFailure {});
                return;
            }
        }

        taken.state.visit(
            [&](FetchByteChannel::Open) { (void)m_channel->arm_waiter(FetchByteChannel::BufferedAtLeast { 1 }); },
            [&](FetchByteChannel::Closed) { finish(move(m_bytes)); },
            [&](FetchByteChannel::Errored const&) { finish(WholeBodyReadFailure {}); },
            [&](FetchByteChannel::ConsumerCancelled) { finish(WholeBodyReadFailure {}); },
            [&](FetchByteChannel::Transferred) { finish(WholeBodyReadFailure {}); });
    }

    void finish(WholeBodyReadResult result)
    {
        m_channel = nullptr;
        auto steps = move(m_steps);
        steps(move(result));
    }

    RefPtr<FetchByteChannel> m_channel;
    Function<void(WholeBodyReadResult)> m_steps;
    ByteBuffer m_bytes;
    bool m_reading { false };
    bool m_woken { false };
};

}

void read_whole_body(Infrastructure::ResponseBody const& body, Function<void(WholeBodyReadResult)> steps)
{
    body.bytes().visit(
        [&](Core::ImmutableBytes const& bytes) {
            auto copy = ByteBuffer::copy(bytes.bytes());
            if (copy.is_error()) {
                steps(WholeBodyReadFailure {});
                return;
            }
            steps(copy.release_value());
        },
        [&](NonnullRefPtr<FetchByteChannel> const& channel) {
            auto read = adopt_ref(*new ChannelRead(channel, move(steps)));
            read->start();
        },
        [](Infrastructure::ResponseBody::ReadThroughStream) { VERIFY_NOT_REACHED(); });
}

}
