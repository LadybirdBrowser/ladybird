/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/NullPlaybackStream.h>

namespace Audio {

NonnullRefPtr<PlaybackStream::CreatePromise> PlaybackStream::create_platform_or_null(OutputState initial_output_state, u32 target_latency_ms, [[maybe_unused]] AudioDataRequestCallback platform_data_request_callback, AudioDataRequestCallback fallback_data_request_callback)
{
    auto promise = PlaybackStream::CreatePromise::construct();

    auto fall_back_to_null = [promise, data_request_callback = move(fallback_data_request_callback), initial_output_state, target_latency_ms](Error& error) mutable {
        warnln("Failed to create playback stream: {}; falling back to null output", error);
        promise->resolve(NullPlaybackStream::create(initial_output_state, target_latency_ms, move(data_request_callback)));
    };

    // A process that reaches an AudioServer never opens a device of its own.
    if (ClientConnection::has_transport_factory()) {
        auto connection = ClientConnection::acquire();
        if (connection.is_error()) {
            fall_back_to_null(connection.error());
            return promise;
        }
        auto remote_promise = connection.value()->create_stream(initial_output_state, target_latency_ms, move(platform_data_request_callback));
        remote_promise->when_resolved([promise](NonnullRefPtr<PlaybackStream>& stream) {
            promise->resolve(stream);
        });
        remote_promise->when_rejected(move(fall_back_to_null));
        return promise;
    }

#if defined(LIBMEDIA_AUDIO_BACKEND)
    auto platform_promise = create_platform_playback_stream(initial_output_state, target_latency_ms, move(platform_data_request_callback));
    platform_promise->when_resolved([promise](NonnullRefPtr<PlaybackStream>& stream) {
        promise->resolve(stream);
    });
    platform_promise->when_rejected(move(fall_back_to_null));
#else
    auto no_backend = Error::from_string_literal("No platform audio backend");
    fall_back_to_null(no_backend);
#endif
    return promise;
}

}
