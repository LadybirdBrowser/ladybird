/*
 * Copyright (c) 2023-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/NullPlaybackStream.h>

namespace Audio {

NonnullRefPtr<PlaybackStream::CreatePromise> PlaybackStream::create(OutputState initial_output_state, u32 target_latency_ms, AudioDataRequestCallback stream_data_request_callback, AudioDataRequestCallback null_fallback_data_request_callback)
{
    auto promise = PlaybackStream::CreatePromise::construct();

    auto fall_back_to_null = [promise, data_request_callback = move(null_fallback_data_request_callback), initial_output_state, target_latency_ms](Error& error) mutable {
        warnln("Failed to create playback stream: {}; falling back to null output", error);
        promise->resolve(NullPlaybackStream::create(initial_output_state, target_latency_ms, move(data_request_callback)));
    };

    // Only the AudioServer opens devices. A process with no server to reach plays into nothing.
    if (!ClientConnection::has_transport_factory()) {
        auto no_server = Error::from_string_literal("No AudioServer to reach");
        fall_back_to_null(no_server);
        return promise;
    }

    auto connection = ClientConnection::acquire();
    if (connection.is_error()) {
        fall_back_to_null(connection.error());
        return promise;
    }
    auto remote_promise = connection.value()->create_stream(initial_output_state, target_latency_ms, move(stream_data_request_callback));
    remote_promise->when_resolved([promise](NonnullRefPtr<PlaybackStream>& stream) {
        promise->resolve(stream);
    });
    remote_promise->when_rejected(move(fall_back_to_null));
    return promise;
}

}
