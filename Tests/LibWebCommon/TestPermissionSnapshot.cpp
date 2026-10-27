/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Permissions/Permissions.h>

using namespace Web::Permissions;

static URL::Origin origin_of(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url->origin();
}

static PermissionSnapshot round_trip(PermissionSnapshot const& snapshot)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(snapshot));
    Queue<IPC::Attachment> attachments;
    for (auto& attachment : message.take_attachments())
        attachments.enqueue(move(attachment));
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return MUST(decoder.decode<PermissionSnapshot>());
}

TEST_CASE(snapshot_round_trips_through_ipc)
{
    PermissionSnapshot snapshot;
    snapshot.revision = 42;
    snapshot.geolocation_default = EffectiveState::Denied;
    snapshot.microphone_default = EffectiveState::Prompt;
    snapshot.entries.append({ origin_of("https://maps.example.com/"sv), PermissionName::Geolocation, EffectiveState::Granted });
    snapshot.entries.append({ origin_of("https://talk.example.org/"sv), PermissionName::Microphone, EffectiveState::Denied });

    auto decoded = round_trip(snapshot);

    EXPECT_EQ(decoded.revision, 42u);
    EXPECT(decoded.geolocation_default == EffectiveState::Denied);
    EXPECT(decoded.microphone_default == EffectiveState::Prompt);
    VERIFY(decoded.entries.size() == 2);
    EXPECT(decoded.entries[0].top_level_origin.is_same_origin(origin_of("https://maps.example.com/"sv)));
    EXPECT(decoded.entries[0].name == PermissionName::Geolocation);
    EXPECT(decoded.entries[0].state == EffectiveState::Granted);
    EXPECT(decoded.entries[1].top_level_origin.is_same_origin(origin_of("https://talk.example.org/"sv)));
    EXPECT(decoded.entries[1].name == PermissionName::Microphone);
    EXPECT(decoded.entries[1].state == EffectiveState::Denied);
}

TEST_CASE(permission_names_round_trip)
{
    EXPECT(to_string(PermissionName::Geolocation) == "geolocation"sv);
    EXPECT(to_string(PermissionName::Microphone) == "microphone"sv);
    EXPECT(permission_name_from_string("geolocation"sv) == PermissionName::Geolocation);
    EXPECT(permission_name_from_string("microphone"sv) == PermissionName::Microphone);
    EXPECT(!permission_name_from_string("camera"sv).has_value());
    EXPECT(!permission_name_from_string("notifications"sv).has_value());
}
