/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::HTML {

struct WEBCOMMON_API BroadcastChannelMessage {
    StorageAPI::StorageKey storage_key;
    Utf16FlyString channel_name;
    URL::Origin source_origin;
    IPCSerializationRecord serialized_message;
    // AD-HOC: Cross-process shared memory backing the SharedArrayBuffers in serialized_message, by file descriptor
    //         — referenced by index from the record, whose bytes can't carry one. So the browser process's fan-out
    //         hands every receiving process the same memory. See SerializedTransferRecord.
    Vector<Core::AnonymousBuffer> shared_buffers;
    pid_t source_process_id { -1 };
    u64 source_channel_id { 0 };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::BroadcastChannelMessage const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::BroadcastChannelMessage> decode(Decoder&);

}
