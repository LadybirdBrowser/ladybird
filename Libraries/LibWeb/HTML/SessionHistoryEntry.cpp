/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SameDocumentNavigationEntry.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

static Utf16String generate_random_uuid_utf16()
{
    auto uuid = Crypto::generate_random_uuid();
    return Utf16String::from_ascii_without_validation(uuid.bytes());
}

NonnullRefPtr<SessionHistoryEntry> SessionHistoryEntry::create()
{
    return adopt_ref(*new SessionHistoryEntry());
}

SessionHistoryEntry::~SessionHistoryEntry() = default;

RefPtr<DocumentState> SessionHistoryEntry::document_state() const { return m_document_state; }
void SessionHistoryEntry::set_document_state(RefPtr<DocumentState> document_state) { m_document_state = move(document_state); }

SessionHistoryEntry::SessionHistoryEntry()
    : m_classic_history_api_state(MUST(structured_serialize_for_storage(JS::VM::the(), JS::js_null())))
    , m_navigation_api_state(MUST(structured_serialize_for_storage(JS::VM::the(), JS::js_undefined())))
    , m_navigation_api_key(generate_random_uuid_utf16())
    , m_navigation_api_id(generate_random_uuid_utf16())
{
}

static SessionHistoryDocumentStateDescriptor create_session_history_document_state_descriptor(DocumentState& document_state)
{
    return {
        .id = document_state.cross_process_id(),
        .history_policy_container = document_state.history_policy_container(),
        .request_referrer = document_state.request_referrer(),
        .request_referrer_policy = document_state.request_referrer_policy(),
        .initiator_origin = document_state.initiator_origin(),
        .origin = document_state.origin(),
        .about_base_url = document_state.about_base_url(),
        .resource = document_state.resource(),
        .reload_pending = document_state.reload_pending(),
        .ever_populated = document_state.ever_populated(),
        .navigable_target_name = document_state.navigable_target_name(),
        .nested_histories = {},
    };
}

SessionHistoryEntryDescriptor create_session_history_entry_descriptor(SessionHistoryEntry const& entry)
{
    auto entry_step = entry.step_value();
    VERIFY(entry_step.has_value());
    SessionHistoryDocumentStateDescriptor document_state_descriptor;
    if (auto document_state = entry.document_state())
        document_state_descriptor = create_session_history_document_state_descriptor(*document_state);

    return {
        .step = static_cast<i32>(*entry_step),
        .url = entry.url(),
        .document_state = move(document_state_descriptor),
        .classic_history_api_state = entry.classic_history_api_state(),
        .navigation_api_state = entry.navigation_api_state(),
        .navigation_api_key = entry.navigation_api_key(),
        .navigation_api_id = entry.navigation_api_id(),
        .scroll_restoration_mode = entry.scroll_restoration_mode(),
        .scroll_position_data = entry.scroll_position_data(),
    };
}

Optional<SessionHistoryEntryPersistedState> create_session_history_entry_persisted_state(SessionHistoryEntry const& entry)
{
    auto document_state = entry.document_state();
    if (!document_state)
        return {};

    return SessionHistoryEntryPersistedState {
        .document_state_id = document_state->cross_process_id(),
        .navigation_api_key = entry.navigation_api_key(),
        .scroll_position_data = entry.scroll_position_data(),
    };
}

SessionHistoryEntryIdentity session_history_entry_identity(SessionHistoryEntry const& entry)
{
    auto document_state = entry.document_state();
    VERIFY(document_state);
    return {
        .document_state_id = document_state->cross_process_id(),
        .navigation_api_id = entry.navigation_api_id(),
    };
}

SessionHistoryEntryIdentity session_history_entry_identity(SessionHistoryEntryDescriptor const& entry)
{
    return {
        .document_state_id = entry.document_state.id,
        .navigation_api_id = entry.navigation_api_id,
    };
}

PendingSessionHistoryEntryDescriptor create_pending_session_history_entry_descriptor(SessionHistoryEntry const& entry)
{
    VERIFY(!entry.step_value().has_value());
    SessionHistoryDocumentStateDescriptor document_state_descriptor;
    if (auto document_state = entry.document_state())
        document_state_descriptor = create_session_history_document_state_descriptor(*document_state);

    return {
        .url = entry.url(),
        .document_state = move(document_state_descriptor),
        .classic_history_api_state = entry.classic_history_api_state(),
        .navigation_api_state = entry.navigation_api_state(),
        .navigation_api_key = entry.navigation_api_key(),
        .navigation_api_id = entry.navigation_api_id(),
        .scroll_restoration_mode = entry.scroll_restoration_mode(),
        .scroll_position_data = entry.scroll_position_data(),
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

SameDocumentNavigationEntry create_same_document_navigation_entry(SessionHistoryEntry const& entry)
{
    auto document_state = entry.document_state();
    VERIFY(document_state);
    return {
        .url = entry.url(),
        .document_state_id = document_state->cross_process_id(),
        .classic_history_api_state = entry.classic_history_api_state(),
        .navigation_api_state = entry.navigation_api_state(),
        .navigation_api_key = entry.navigation_api_key(),
        .navigation_api_id = entry.navigation_api_id(),
        .scroll_restoration_mode = entry.scroll_restoration_mode(),
        .scroll_position_data = entry.scroll_position_data(),
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
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SameDocumentNavigationEntry const& entry)
{
    TRY(encoder.encode(entry.url));
    TRY(encoder.encode(entry.document_state_id));
    TRY(encoder.encode(entry.classic_history_api_state));
    TRY(encoder.encode(entry.navigation_api_state));
    TRY(encoder.encode(entry.navigation_api_key));
    TRY(encoder.encode(entry.navigation_api_id));
    TRY(encoder.encode(entry.scroll_restoration_mode));
    TRY(encoder.encode(entry.scroll_position_data));
    return {};
}

template<>
ErrorOr<Web::HTML::SameDocumentNavigationEntry> IPC::decode(Decoder& decoder)
{
    return Web::HTML::SameDocumentNavigationEntry {
        .url = TRY(decoder.decode<URL::URL>()),
        .document_state_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
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
    TRY(encoder.encode(persisted_state.document_state_id));
    TRY(encoder.encode(persisted_state.navigation_api_key));
    TRY(encoder.encode(persisted_state.scroll_position_data));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryEntryPersistedState> IPC::decode(Decoder& decoder)
{
    return Web::HTML::SessionHistoryEntryPersistedState {
        .document_state_id = TRY(decoder.decode<Web::HTML::CrossProcessId>()),
        .navigation_api_key = TRY(decoder.decode<Utf16String>()),
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
    TRY(encoder.encode(document_state.nested_histories));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryDocumentStateDescriptor> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::HTML::CrossProcessId>());
    auto history_policy_container = TRY(decoder.decode<Variant<Web::HTML::SerializedPolicyContainer, Web::HTML::DocumentState::Client>>());
    auto request_referrer = TRY(decoder.decode<Web::Fetch::Infrastructure::Request::ReferrerType>());
    auto request_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());
    auto initiator_origin = TRY(decoder.decode<Optional<URL::Origin>>());
    auto origin = TRY(decoder.decode<Optional<URL::Origin>>());
    auto about_base_url = TRY(decoder.decode<Optional<URL::URL>>());
    auto resource = TRY(decoder.decode<Web::HTML::DocumentResource>());
    auto reload_pending = TRY(decoder.decode<bool>());
    auto ever_populated = TRY(decoder.decode<bool>());
    auto navigable_target_name = TRY(decoder.decode<Utf16String>());
    auto nested_histories = TRY(decoder.decode<Vector<Web::HTML::SessionHistoryNestedHistoryDescriptor>>());

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
        .nested_histories = move(nested_histories),
    };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::SessionHistoryNestedHistoryDescriptor const& nested_history)
{
    TRY(encoder.encode(nested_history.id));
    TRY(encoder.encode(nested_history.entries));
    return {};
}

template<>
ErrorOr<Web::HTML::SessionHistoryNestedHistoryDescriptor> IPC::decode(Decoder& decoder)
{
    auto id = TRY(decoder.decode<Web::HTML::CrossProcessId>());
    auto entries = TRY(decoder.decode<Vector<Web::HTML::SessionHistoryEntryDescriptor>>());

    return Web::HTML::SessionHistoryNestedHistoryDescriptor { id, move(entries) };
}
