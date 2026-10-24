/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Video/VideoFramePresentationPage.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMediaCodecs/Limits.h>

namespace Compositing {

static_assert(__atomic_always_lock_free(sizeof(u64), nullptr));

static constexpr size_t max_compare_exchange_attempts_before_assuming_a_misbehaving_peer = 64;

ErrorOr<NonnullRefPtr<VideoFramePresentationPage>> VideoFramePresentationPage::create()
{
    auto buffer = TRY(Core::AnonymousBuffer::create_with_size(sizeof(SharedState)));
    new (buffer.data<void>()) SharedState;
    return create(move(buffer));
}

ErrorOr<NonnullRefPtr<VideoFramePresentationPage>> VideoFramePresentationPage::create(Core::AnonymousBuffer buffer)
{
    if (!buffer.is_valid() || buffer.size() < sizeof(SharedState))
        return Error::from_string_literal("Invalid video frame presentation page buffer");
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFramePresentationPage(move(buffer)));
}

VideoFramePresentationPage::VideoFramePresentationPage(Core::AnonymousBuffer buffer)
    : m_buffer(move(buffer))
    , m_shared_state(reinterpret_cast<SharedState*>(m_buffer.data<void>()))
{
}

u64 VideoFramePresentationPage::pack(GenerationAndHolds value)
{
    VERIFY(value.generation <= retired_generation);
    VERIFY(value.hold_count <= max_hold_count);
    return (value.generation << 16) | value.hold_count;
}

VideoFramePresentationPage::GenerationAndHolds VideoFramePresentationPage::unpack(u64 word)
{
    return { .generation = word >> 16, .hold_count = word & max_hold_count };
}

// Every change to the word is a read-modify-write, and it guards no other data, so nothing needs ordering.

VideoFramePresentationPage::GenerationAndHolds VideoFramePresentationPage::replace_generation(u64 new_generation)
{
    return unpack(word().exchange(pack({ .generation = new_generation, .hold_count = 0 }), AK::MemoryOrder::memory_order_relaxed));
}

u64 VideoFramePresentationPage::current_generation() const
{
    return unpack(word().load(AK::MemoryOrder::memory_order_relaxed)).generation;
}

Optional<u64> VideoFramePresentationPage::hold_current_generation()
{
    return hold(OptionalNone {});
}

bool VideoFramePresentationPage::hold_generation(u64 generation)
{
    return hold(generation).has_value();
}

Optional<u64> VideoFramePresentationPage::hold(Optional<u64> required_generation)
{
    auto expected = word().load(AK::MemoryOrder::memory_order_relaxed);
    for (size_t attempt = 0; attempt < max_compare_exchange_attempts_before_assuming_a_misbehaving_peer; attempt++) {
        auto current = unpack(expected);
        if (current.generation == retired_generation || current.hold_count == max_hold_count)
            return {};
        if (required_generation.has_value() && current.generation != required_generation.value())
            return {};
        auto held = pack({ .generation = current.generation, .hold_count = current.hold_count + 1 });
        if (word().compare_exchange_strong(expected, held, AK::MemoryOrder::memory_order_relaxed))
            return current.generation;
    }
    return {};
}

void VideoFramePresentationPage::publish_presented_frame(PresentedFrame const& presented_frame)
{
    m_shared_state->presented_frame.store(presented_frame);
}

Optional<VideoFramePresentationPage::PresentedFrame> VideoFramePresentationPage::presented_frame() const
{
    auto presented_frame = m_shared_state->presented_frame.read();
    if (!presented_frame.has_value())
        return {};
    if (presented_frame->width > Media::MAX_VIDEO_FRAME_DIMENSION || presented_frame->height > Media::MAX_VIDEO_FRAME_DIMENSION)
        return {};
    return presented_frame;
}

VideoFramePresentationPage::ReleaseResult VideoFramePresentationPage::release_hold_if_current(u64 generation)
{
    auto expected = word().load(AK::MemoryOrder::memory_order_relaxed);
    for (size_t attempt = 0; attempt < max_compare_exchange_attempts_before_assuming_a_misbehaving_peer; attempt++) {
        auto current = unpack(expected);
        if (current.generation != generation)
            return ReleaseResult::NotCurrent;
        if (current.hold_count == 0)
            return ReleaseResult::NotHeld;
        auto released = pack({ .generation = current.generation, .hold_count = current.hold_count - 1 });
        if (word().compare_exchange_strong(expected, released, AK::MemoryOrder::memory_order_relaxed))
            return ReleaseResult::Released;
    }
    return ReleaseResult::NotHeld;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, NonnullRefPtr<Compositing::VideoFramePresentationPage> const& page)
{
    return encoder.encode(page->buffer());
}

template<>
ErrorOr<NonnullRefPtr<Compositing::VideoFramePresentationPage>> decode(Decoder& decoder)
{
    return Compositing::VideoFramePresentationPage::create(TRY(decoder.decode<Core::AnonymousBuffer>()));
}

}
