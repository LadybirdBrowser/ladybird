/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/NotificationsAPI/NotificationData.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::NotificationsAPI::NotificationData const& data)
{
    TRY(encoder.encode(data.title));
    TRY(encoder.encode(data.body));
    TRY(encoder.encode(data.tag));
    TRY(encoder.encode(data.language));
    TRY(encoder.encode(data.direction));
    TRY(encoder.encode(data.silent));
    TRY(encoder.encode(data.require_interaction));
    TRY(encoder.encode(data.renotify));
    return {};
}

template<>
ErrorOr<Web::NotificationsAPI::NotificationData> IPC::decode(Decoder& decoder)
{
    Web::NotificationsAPI::NotificationData data;
    data.title = TRY(decoder.decode<Utf16String>());
    data.body = TRY(decoder.decode<Utf16String>());
    data.tag = TRY(decoder.decode<Utf16String>());
    data.language = TRY(decoder.decode<Utf16String>());
    data.direction = TRY(decoder.decode<Web::Bindings::NotificationDirection>());
    data.silent = TRY(decoder.decode<Optional<bool>>());
    data.require_interaction = TRY(decoder.decode<bool>());
    data.renotify = TRY(decoder.decode<bool>());
    return data;
}
