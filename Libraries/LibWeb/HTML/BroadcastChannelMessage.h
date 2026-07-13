/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16FlyString.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::HTML {

struct WEB_API BroadcastChannelMessage {
    StorageAPI::StorageKey storage_key;
    Utf16FlyString channel_name;
    URL::Origin source_origin;
    IPCSerializationRecord serialized_message;
    pid_t source_process_id { -1 };
    u64 source_channel_id { 0 };
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::BroadcastChannelMessage const&);

template<>
WEB_API ErrorOr<Web::HTML::BroadcastChannelMessage> decode(Decoder&);

}
