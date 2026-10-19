/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/Video/VideoFramePresentationPage.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace Compositing {

static_assert(__atomic_always_lock_free(sizeof(u64), nullptr));

static constexpr size_t max_compare_exchange_attempts_before_assuming_a_misbehaving_peer = 64;

ErrorOr<NonnullRefPtr<VideoFramePresentationPage>> VideoFramePresentationPage::create()
{
    return create(TRY(Core::AnonymousBuffer::create_with_size(sizeof(Atomic<u64>))));
}

ErrorOr<NonnullRefPtr<VideoFramePresentationPage>> VideoFramePresentationPage::create(Core::AnonymousBuffer buffer)
{
    if (!buffer.is_valid() || buffer.size() < sizeof(Atomic<u64>))
        return Error::from_string_literal("Invalid video frame presentation page buffer");
    return adopt_nonnull_ref_or_enomem(new (nothrow) VideoFramePresentationPage(move(buffer)));
}

VideoFramePresentationPage::VideoFramePresentationPage(Core::AnonymousBuffer buffer)
    : m_buffer(move(buffer))
    , m_word(reinterpret_cast<Atomic<u64>*>(m_buffer.data<void>()))
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
    auto expected = word().load(AK::MemoryOrder::memory_order_relaxed);
    for (size_t attempt = 0; attempt < max_compare_exchange_attempts_before_assuming_a_misbehaving_peer; attempt++) {
        auto current = unpack(expected);
        if (current.generation == retired_generation || current.hold_count == max_hold_count)
            return {};
        auto held = pack({ .generation = current.generation, .hold_count = current.hold_count + 1 });
        if (word().compare_exchange_strong(expected, held, AK::MemoryOrder::memory_order_relaxed))
            return current.generation;
    }
    return {};
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
