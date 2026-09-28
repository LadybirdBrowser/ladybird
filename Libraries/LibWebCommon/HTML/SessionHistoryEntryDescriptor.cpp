/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/SessionHistoryEntryDescriptor.h>

namespace Web::HTML {

static Utf16String generate_random_uuid_utf16()
{
    auto uuid = generate_random_uuid();
    return Utf16String::from_ascii_without_validation(uuid.bytes());
}

SessionHistoryEntryDescriptor create_initial_session_history_entry_descriptor(CrossProcessId document_state_id, Optional<URL::URL> about_base_url, Utf16String navigable_target_name)
{
    // NB: The origin fields are set from the document once it is created.
    return {
        .step = 0,
        .url = URL::about_blank(),
        .document_state = {
            .id = document_state_id,
            .history_policy_container = DocumentStateClient::Tag,
            .request_referrer = Fetch::Infrastructure::RequestReferrer::Client,
            .request_referrer_policy = ReferrerPolicy::DEFAULT_REFERRER_POLICY,
            .initiator_origin = {},
            .origin = {},
            .about_base_url = move(about_base_url),
            .resource = Empty {},
            .reload_pending = false,
            .ever_populated = false,
            .navigable_target_name = move(navigable_target_name),
            .nested_histories = {},
        },
        .classic_history_api_state = {},
        .navigation_api_state = {},
        .navigation_api_key = generate_random_uuid_utf16(),
        .navigation_api_id = generate_random_uuid_utf16(),
        .scroll_restoration_mode = ScrollRestorationMode::Auto,
        .scroll_position_data = {},
    };
}

SessionHistoryEntryIdentity session_history_entry_identity(SessionHistoryEntryDescriptor const& entry)
{
    return {
        .document_state_id = entry.document_state.id,
        .navigation_api_id = entry.navigation_api_id,
    };
}

PendingSessionHistoryEntryDescriptor create_pending_session_history_entry_descriptor(SessionHistoryEntryDescriptor entry)
{
    return {
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
    };
}

SessionHistoryEntryDescriptor create_session_history_entry_descriptor(PendingSessionHistoryEntryDescriptor entry, i32 step)
{
    return {
        .step = step,
        .url = move(entry.url),
        .document_state = move(entry.document_state),
        .classic_history_api_state = move(entry.classic_history_api_state),
        .navigation_api_state = move(entry.navigation_api_state),
        .navigation_api_key = move(entry.navigation_api_key),
        .navigation_api_id = move(entry.navigation_api_id),
        .scroll_restoration_mode = entry.scroll_restoration_mode,
        .scroll_position_data = move(entry.scroll_position_data),
    };
}

}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SessionHistoryEntryDescriptor const& entry)
{
    TRY(encoder.encode(entry.step));
    TRY(encoder.encode(entry.url));
    TRY(encoder.encode(entry.document_state));
    TRY(encoder.encode(entry.classic_history_api_state));
    TRY(encoder.encode(entry.navigation_api_state));
    TRY(encoder.encode(entry.navigation_api_key));
    TRY(encoder.encode(entry.navigation_api_id));
    TRY(encoder.encode(entry.scroll_restoration_mode));
    TRY(encoder.encode(entry.scroll_position_data));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryEntryDescriptor> IPC::decode(Decoder& decoder)
{
    auto step = TRY(decoder.decode<i32>());
    auto url = TRY(decoder.decode<URL::URL>());
    auto document_state = TRY(decoder.decode<Web::HTML::SessionHistoryDocumentStateDescriptor>());
    auto classic_history_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>());
    auto navigation_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>());
    auto navigation_api_key = TRY(decoder.decode<Utf16String>());
    auto navigation_api_id = TRY(decoder.decode<Utf16String>());
    auto scroll_restoration_mode = TRY(decoder.decode<Web::HTML::ScrollRestorationMode>());
    auto scroll_position_data = TRY(decoder.decode<Web::HTML::SessionHistoryEntryScrollPositionData>());

    return Web::HTML::SessionHistoryEntryDescriptor {
        .step = step,
        .url = move(url),
        .document_state = move(document_state),
        .classic_history_api_state = move(classic_history_api_state),
        .navigation_api_state = move(navigation_api_state),
        .navigation_api_key = move(navigation_api_key),
        .navigation_api_id = move(navigation_api_id),
        .scroll_restoration_mode = scroll_restoration_mode,
        .scroll_position_data = move(scroll_position_data),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::PendingSessionHistoryEntryDescriptor const& entry)
{
    TRY(encoder.encode(entry.url));
    TRY(encoder.encode(entry.document_state));
    TRY(encoder.encode(entry.classic_history_api_state));
    TRY(encoder.encode(entry.navigation_api_state));
    TRY(encoder.encode(entry.navigation_api_key));
    TRY(encoder.encode(entry.navigation_api_id));
    TRY(encoder.encode(entry.scroll_restoration_mode));
    TRY(encoder.encode(entry.scroll_position_data));
    return {};
}

template<>
ErrorOr<Web::HTML::PendingSessionHistoryEntryDescriptor> IPC::decode(Decoder& decoder)
{
    return Web::HTML::PendingSessionHistoryEntryDescriptor {
        .url = TRY(decoder.decode<URL::URL>()),
        .document_state = TRY(decoder.decode<Web::HTML::SessionHistoryDocumentStateDescriptor>()),
        .classic_history_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>()),
        .navigation_api_state = TRY(decoder.decode<Web::HTML::StorageSerializationRecord>()),
        .navigation_api_key = TRY(decoder.decode<Utf16String>()),
        .navigation_api_id = TRY(decoder.decode<Utf16String>()),
        .scroll_restoration_mode = TRY(decoder.decode<Web::HTML::ScrollRestorationMode>()),
        .scroll_position_data = TRY(decoder.decode<Web::HTML::SessionHistoryEntryScrollPositionData>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SessionHistoryEntryScrollPositionData const& scroll_position_data)
{
    TRY(encoder.encode(scroll_position_data.viewport_scroll_position));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryEntryScrollPositionData> IPC::decode(Decoder& decoder)
{
    auto viewport_scroll_position = TRY(decoder.decode<Optional<Web::CSSPixelPoint>>());
    return Web::HTML::SessionHistoryEntryScrollPositionData {
        .viewport_scroll_position = move(viewport_scroll_position),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SessionHistoryEntryPersistedState const& persisted_state)
{
    TRY(encoder.encode(persisted_state.entry_identity));
    TRY(encoder.encode(persisted_state.scroll_position_data));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryEntryPersistedState> IPC::decode(Decoder& decoder)
{
    return Web::HTML::SessionHistoryEntryPersistedState {
        .entry_identity = TRY(decoder.decode<Web::HTML::SessionHistoryEntryIdentity>()),
        .scroll_position_data = TRY(decoder.decode<Web::HTML::SessionHistoryEntryScrollPositionData>()),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SessionHistoryDocumentStateDescriptor const& document_state)
{
    TRY(encoder.encode(document_state.id));
    TRY(encoder.encode(document_state.history_policy_container));
    TRY(encoder.encode(document_state.request_referrer));
    TRY(encoder.encode(document_state.request_referrer_policy));
    TRY(encoder.encode(document_state.initiator_origin));
    TRY(encoder.encode(document_state.origin));
    TRY(encoder.encode(document_state.about_base_url));
    TRY(encoder.encode(document_state.resource));
    TRY(encoder.encode(document_state.reload_pending));
    TRY(encoder.encode(document_state.ever_populated));
    TRY(encoder.encode(document_state.navigable_target_name));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryDocumentStateDescriptor> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::HTML::CrossProcessId>());
    auto history_policy_container = TRY(decoder.decode<Variant<Web::HTML::SerializedPolicyContainer, Web::HTML::DocumentStateClient>>());
    auto request_referrer = TRY(decoder.decode<Web::Fetch::Infrastructure::RequestReferrerType>());
    auto request_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());
    auto initiator_origin = TRY(decoder.decode<Optional<URL::Origin>>());
    auto origin = TRY(decoder.decode<Optional<URL::Origin>>());
    auto about_base_url = TRY(decoder.decode<Optional<URL::URL>>());
    auto resource = TRY(decoder.decode<Web::HTML::DocumentResource>());
    auto reload_pending = TRY(decoder.decode<bool>());
    auto ever_populated = TRY(decoder.decode<bool>());
    auto navigable_target_name = TRY(decoder.decode<Utf16String>());

    return Web::HTML::SessionHistoryDocumentStateDescriptor {
        .id = id,
        .history_policy_container = move(history_policy_container),
        .request_referrer = move(request_referrer),
        .request_referrer_policy = request_referrer_policy,
        .initiator_origin = move(initiator_origin),
        .origin = move(origin),
        .about_base_url = move(about_base_url),
        .resource = move(resource),
        .reload_pending = reload_pending,
        .ever_populated = ever_populated,
        .navigable_target_name = move(navigable_target_name),
        .nested_histories = {},
    };
}
