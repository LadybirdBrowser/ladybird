/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/RecordStream.h>

namespace Audio {

NonnullRefPtr<RecordStream::CreatePromise> RecordStream::create(SampleSpecification const& specification, u32 fragment_size_bytes, StringView device_id, RecordCallback callback)
{
    // A process that reaches an AudioServer never opens a device of its own.
    if (ClientConnection::has_transport_factory()) {
        auto connection = ClientConnection::acquire();
        if (connection.is_error())
            return CreatePromise::rejected(connection.release_error());
        return connection.value()->create_record_stream(device_id, move(callback));
    }
    return create_platform(specification, fragment_size_bytes, device_id, move(callback));
}

#if !defined(LIBMEDIA_AUDIO_CAPTURE_BACKEND)

NonnullRefPtr<RecordStream::CreatePromise> RecordStream::create_platform(SampleSpecification const&, u32, StringView, RecordCallback)
{
    return CreatePromise::rejected(Error::from_string_literal("Audio capture is not supported on this platform"));
}

#endif

}
