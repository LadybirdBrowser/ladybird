/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Types.h>

namespace Web::WebAudio::Render {

// https://webaudio.github.io/web-audio-api/#render-quantum-size
// FIXME: 128 is a terrific number, but there may be others out there.
inline constexpr size_t RENDER_QUANTUM_SIZE { 128 };

inline constexpr u32 SCRIPT_PROCESSOR_PUBLISH_RETRY_INTERVAL_MS { 10 };
inline constexpr u32 SCRIPT_PROCESSOR_PUBLISH_RETRY_MAX_ATTEMPTS { 200 };
inline constexpr i64 REALTIME_SCRIPT_PROCESSOR_HOST_WAIT_TIMEOUT_MS { 250 };

inline constexpr u32 AUDIO_CONTEXT_RENDER_THREAD_STATE_ACK_POLL_INTERVAL_MS { 5 };
inline constexpr u32 AUDIO_CONTEXT_INTERACTIVE_TARGET_LATENCY_MS { 20 };
inline constexpr u32 AUDIO_CONTEXT_BALANCED_TARGET_LATENCY_MS { 50 };
inline constexpr u32 AUDIO_CONTEXT_PLAYBACK_TARGET_LATENCY_MS { 200 };
inline constexpr u32 AUDIO_CONTEXT_MAX_SUPPORTED_TARGET_LATENCY_MS { 500 };

inline constexpr u32 WEBAUDIO_WORKER_MAX_QUEUED_MS_WITH_MEDIA_ELEMENT_SOURCE { 30 };
inline constexpr u32 WEBAUDIO_WORKER_MAX_QUEUED_MS_DEFAULT { 200 };

}
