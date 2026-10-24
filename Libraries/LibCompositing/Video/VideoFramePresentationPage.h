/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/SeqLock.h>
#include <AK/SharedMemoryValue.h>
#include <AK/Time.h>
#include <LibCompositing/Export.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>

namespace Compositing {

// The generation and its hold count share one word, so a hold is either counted before the Compositor replaces the
// generation, or fails and is retried on the new one.
class COMPOSITING_API VideoFramePresentationPage : public RefCounted<VideoFramePresentationPage> {
public:
    static constexpr u64 retired_generation = (1ull << 48) - 1;
    static constexpr u64 max_hold_count = (1ull << 16) - 1;

    static ErrorOr<NonnullRefPtr<VideoFramePresentationPage>> create();
    static ErrorOr<NonnullRefPtr<VideoFramePresentationPage>> create(Core::AnonymousBuffer);

    Core::AnonymousBuffer const& buffer() const { return m_buffer; }

    struct GenerationAndHolds {
        u64 generation { 0 };
        u64 hold_count { 0 };
    };

    GenerationAndHolds replace_generation(u64 new_generation);

    u64 current_generation() const;
    Optional<u64> hold_current_generation();

    enum class ReleaseResult : u8 {
        Released,
        NotCurrent,
        NotHeld,
    };
    ReleaseResult release_hold_if_current(u64 generation);

    struct PresentedFrame {
        u64 generation { 0 };
        u64 presented_frame_count { 0 };
        u64 dropped_frame_count { 0 };
        SharedMemoryValue<AK::Duration> media_time;
        SharedMemoryValue<MonotonicTime> presentation_time;
        u32 width { 0 };
        u32 height { 0 };
    };
    void publish_presented_frame(PresentedFrame const&);
    Optional<PresentedFrame> presented_frame() const;
    bool hold_generation(u64 generation);

private:
    struct SharedState {
        Atomic<u64> word;
        SeqLock<PresentedFrame> presented_frame;
    };

    VideoFramePresentationPage(Core::AnonymousBuffer);

    Optional<u64> hold(Optional<u64> required_generation);

    static u64 pack(GenerationAndHolds);
    static GenerationAndHolds unpack(u64);

    Atomic<u64>& word() const { return m_shared_state->word; }

    Core::AnonymousBuffer m_buffer;
    SharedState* m_shared_state { nullptr };
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, NonnullRefPtr<Compositing::VideoFramePresentationPage> const&);

template<>
COMPOSITING_API ErrorOr<NonnullRefPtr<Compositing::VideoFramePresentationPage>> decode(Decoder&);

}
