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
#include <LibWebCommon/HTML/Scripting/EnvironmentId.h>
#include <LibWebCommon/HTML/SerializationRecords.h>
#include <LibWebCommon/StorageAPI/StorageKey.h>

namespace Web::HTML {

struct WEBCOMMON_API BroadcastChannelMessage {
    StorageAPI::StorageKey storage_key;
    Utf16FlyString channel_name;
    URL::Origin source_origin;
    IPCSerializationRecord serialized_message;
    SharedStoreLifeline shared_store_lifeline;
};

struct WEBCOMMON_API PostedBroadcastChannelMessage {
    EnvironmentId environment_id;
    Utf16FlyString channel_name;
    IPCSerializationRecord serialized_message;
    SharedStoreLifeline shared_store_lifeline;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::BroadcastChannelMessage const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::BroadcastChannelMessage> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::PostedBroadcastChannelMessage const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::PostedBroadcastChannelMessage> decode(Decoder&);

}
