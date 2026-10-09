/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/RecordStream.h>

namespace Audio {

NonnullRefPtr<RecordStream::CreatePromise> RecordStream::create(StringView device_id, RecordCallback callback)
{
    // Only the AudioServer opens devices.
    if (!ClientConnection::has_transport_factory())
        return CreatePromise::rejected(Error::from_string_literal("No AudioServer to reach"));

    auto connection = ClientConnection::acquire();
    if (connection.is_error())
        return CreatePromise::rejected(connection.release_error());
    return connection.value()->create_record_stream(device_id, move(callback));
}

}
