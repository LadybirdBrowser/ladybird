/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Types.h>

namespace Web::WebAudio::Render {

static constexpr u32 webaudio_timing_page_version = 2;

static constexpr u64 webaudio_suspend_state_suspended_bit = 1u;

inline u64 encode_webaudio_suspend_state(bool suspended, u64 generation)
{
    // Pack as: bit0 = suspended, bits[63:1] = generation.
    return (generation << 1) | (suspended ? webaudio_suspend_state_suspended_bit : 0);
}

inline bool decode_webaudio_suspend_state_is_suspended(u64 state)
{
    return (state & webaudio_suspend_state_suspended_bit) != 0;
}

inline u64 decode_webaudio_suspend_state_generation(u64 state)
{
    return state >> 1;
}

static constexpr u32 webaudio_analyser_snapshot_version = 1;

struct WebAudioAnalyserSnapshotHeader {
    u32 version;
    u32 fft_size;
    u64 analyser_node_id;
    u64 rendered_frames_total;
};

static_assert(sizeof(WebAudioAnalyserSnapshotHeader) % alignof(f32) == 0);

inline size_t webaudio_analyser_snapshot_size_bytes(u32 fft_size)
{
    // Payload: header + time-domain floats (fft_size) + frequency dB floats (fft_size / 2).
    return sizeof(WebAudioAnalyserSnapshotHeader)
        + (static_cast<size_t>(fft_size) * sizeof(f32))
        + (static_cast<size_t>(fft_size) / 2 * sizeof(f32));
}

static constexpr u32 webaudio_dynamics_compressor_snapshot_version = 1;

struct WebAudioDynamicsCompressorSnapshotHeader {
    u32 version;
    u64 compressor_node_id;
    u64 rendered_frames_total;
    f32 reduction_db;
};

static_assert(sizeof(WebAudioDynamicsCompressorSnapshotHeader) % alignof(f32) == 0);

inline size_t webaudio_dynamics_compressor_snapshot_size_bytes()
{
    return sizeof(WebAudioDynamicsCompressorSnapshotHeader);
}

struct WebAudioTimingPage {
    u32 sequence;
    u32 version;

    u32 sample_rate_hz;
    u32 channel_count;

    u64 rendered_frames_total;
    u64 underrun_frames_total;
    u64 graph_generation;

    // Packed suspend state (see encode_webaudio_suspend_state()).
    u64 reserved0;
};

struct WebAudioTimingSnapshot {
    u32 version { 0 };
    u32 sample_rate_hz { 0 };
    u32 channel_count { 0 };
    u64 rendered_frames_total { 0 };
    u64 underrun_frames_total { 0 };
    u64 graph_generation { 0 };
    u64 suspend_state { 0 };
};

inline void write_webaudio_timing_page(WebAudioTimingPage& page, u32 sample_rate_hz, u32 channel_count, u64 rendered_frames_total, u64 underrun_frames_total, u64 graph_generation, u64 suspend_state)
{
    u32 seq = AK::atomic_load(&page.sequence, AK::MemoryOrder::memory_order_relaxed);
    seq = seq + 1;
    if ((seq & 1u) == 0)
        seq++;

    AK::atomic_store(&page.sequence, seq, AK::MemoryOrder::memory_order_release);

    page.version = webaudio_timing_page_version;
    page.sample_rate_hz = sample_rate_hz;
    page.channel_count = channel_count;
    page.rendered_frames_total = rendered_frames_total;
    page.underrun_frames_total = underrun_frames_total;
    page.graph_generation = graph_generation;
    page.reserved0 = suspend_state;

    AK::atomic_store(&page.sequence, seq + 1, AK::MemoryOrder::memory_order_release);
}

inline bool read_webaudio_timing_page(WebAudioTimingPage const& page, WebAudioTimingSnapshot& out)
{
    for (size_t attempt = 0; attempt < 4; ++attempt) {
        u32 s0 = AK::atomic_load(&page.sequence, AK::MemoryOrder::memory_order_acquire);
        if ((s0 & 1u) != 0)
            continue;

        WebAudioTimingSnapshot snapshot;
        snapshot.version = page.version;
        snapshot.sample_rate_hz = page.sample_rate_hz;
        snapshot.channel_count = page.channel_count;
        snapshot.rendered_frames_total = page.rendered_frames_total;
        snapshot.underrun_frames_total = page.underrun_frames_total;
        snapshot.graph_generation = page.graph_generation;
        snapshot.suspend_state = page.reserved0;

        u32 s1 = AK::atomic_load(&page.sequence, AK::MemoryOrder::memory_order_acquire);
        if (s0 != s1)
            continue;

        if (snapshot.version != webaudio_timing_page_version)
            return false;

        out = snapshot;
        return true;
    }

    return false;
}

}
