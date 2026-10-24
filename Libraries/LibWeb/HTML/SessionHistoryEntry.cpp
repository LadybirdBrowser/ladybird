/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibJS/Runtime/Value.h>
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

static CrossProcessId generate_directive_state_id()
{
    auto uuid = Crypto::generate_random_uuid();
    u64 namespace_id = 0;
    u64 local_id = 0;
    size_t digit_count = 0;
    for (auto byte : uuid.bytes()) {
        if (byte == '-')
            continue;
        auto const nibble = parse_ascii_hex_digit(byte);
        if (digit_count < 16)
            namespace_id = (namespace_id << 4) | nibble;
        else
            local_id = (local_id << 4) | nibble;
        ++digit_count;
    }
    VERIFY(digit_count == 32);
    return { namespace_id, local_id };
}

NonnullRefPtr<SessionHistoryEntry> SessionHistoryEntry::create()
{
    return create(DirectiveState::create(generate_directive_state_id()));
}

NonnullRefPtr<SessionHistoryEntry> SessionHistoryEntry::create(NonnullRefPtr<DirectiveState> directive_state)
{
    return adopt_ref(*new SessionHistoryEntry(move(directive_state)));
}

SessionHistoryEntry::~SessionHistoryEntry() = default;

RefPtr<DocumentState> SessionHistoryEntry::document_state() const { return m_document_state; }
void SessionHistoryEntry::set_document_state(RefPtr<DocumentState> document_state) { m_document_state = move(document_state); }

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-document
UniqueNodeID SessionHistoryEntry::document_id() const
{
    // To get a session history entry's document, return its document state's document.
    VERIFY(m_document_state);
    auto id = m_document_state->document_id();
    VERIFY(id.has_value());
    return id.release_value();
}

SessionHistoryEntry::SessionHistoryEntry()
    : SessionHistoryEntry(DirectiveState::create(generate_directive_state_id()))
{
}

SessionHistoryEntry::SessionHistoryEntry(NonnullRefPtr<DirectiveState> directive_state)
    : m_classic_history_api_state(structured_serialize_undefined_or_null_for_storage(JS::js_null()))
    , m_navigation_api_state(structured_serialize_undefined_or_null_for_storage(JS::js_undefined()))
    , m_navigation_api_key(generate_random_uuid_utf16())
    , m_navigation_api_id(generate_random_uuid_utf16())
    , m_directive_state(move(directive_state))
{
}

SessionHistoryDocumentStateDescriptor create_session_history_document_state_descriptor(DocumentState const& document_state)
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
        .reload_bypasses_cache = document_state.reload_bypasses_cache(),
        .ever_populated = document_state.ever_populated(),
        .user_agent_initiated = document_state.user_agent_initiated(),
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
        .directive_state_id = entry.directive_state()->cross_process_id(),
        .directive_state_value = entry.directive_state()->value(),
    };
}

Optional<SessionHistoryEntryPersistedState> create_session_history_entry_persisted_state(SessionHistoryEntry const& entry)
{
    auto document_state = entry.document_state();
    if (!document_state)
        return {};

    return SessionHistoryEntryPersistedState {
        .entry_identity = session_history_entry_identity(entry),
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
        .directive_state_id = entry.directive_state()->cross_process_id(),
        .directive_state_value = entry.directive_state()->value(),
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
        .directive_state_id = entry.directive_state()->cross_process_id(),
        .directive_state_value = entry.directive_state()->value(),
    };
}

void apply_session_history_entry_descriptor_from_ui_process(SessionHistoryEntry& entry, SessionHistoryEntryDescriptor& entry_descriptor)
{
    entry.set_url(move(entry_descriptor.url));
    entry.set_step(static_cast<int>(entry_descriptor.step));
    entry.set_classic_history_api_state(move(entry_descriptor.classic_history_api_state));
    entry.set_navigation_api_state(move(entry_descriptor.navigation_api_state));
    entry.set_navigation_api_key(move(entry_descriptor.navigation_api_key));
    entry.set_navigation_api_id(move(entry_descriptor.navigation_api_id));
    entry.set_scroll_restoration_mode(entry_descriptor.scroll_restoration_mode);
    entry.set_scroll_position_data(move(entry_descriptor.scroll_position_data));
    if (entry.directive_state()->cross_process_id() == entry_descriptor.directive_state_id)
        entry.directive_state()->set_value(move(entry_descriptor.directive_state_value));
    else
        entry.set_directive_state(DirectiveState::create(entry_descriptor.directive_state_id, move(entry_descriptor.directive_state_value)));
}

void apply_session_history_document_state_descriptor_from_ui_process(DocumentState& document_state, SessionHistoryDocumentStateDescriptor const& document_state_descriptor)
{
    VERIFY(document_state.cross_process_id() == document_state_descriptor.id);
    document_state.set_history_policy_container(document_state_descriptor.history_policy_container);
    document_state.set_request_referrer(document_state_descriptor.request_referrer);
    document_state.set_request_referrer_policy(document_state_descriptor.request_referrer_policy);
    document_state.set_initiator_origin(document_state_descriptor.initiator_origin);
    document_state.set_origin(document_state_descriptor.origin);
    document_state.set_about_base_url(document_state_descriptor.about_base_url);
    document_state.set_resource(document_state_descriptor.resource);
    document_state.set_reload_pending(document_state_descriptor.reload_pending);
    document_state.set_reload_bypasses_cache(document_state_descriptor.reload_bypasses_cache);
    document_state.set_ever_populated(document_state_descriptor.ever_populated);
    document_state.set_user_agent_initiated(document_state_descriptor.user_agent_initiated);
    document_state.set_navigable_target_name(document_state_descriptor.navigable_target_name);
}

RefPtr<DocumentState> get_or_create_document_state_from_ui_process(SessionHistoryDocumentStateDescriptor const& document_state_descriptor, SessionHistoryEntryReconstructionState& reconstruction_state)
{
    RefPtr<DocumentState> document_state;
    if (auto existing_document_state = reconstruction_state.document_states.get(document_state_descriptor.id); existing_document_state.has_value())
        document_state = *existing_document_state;

    if (!document_state) {
        document_state = DocumentState::create(document_state_descriptor.id);
        reconstruction_state.document_states.set(document_state_descriptor.id, document_state);
    }

    apply_session_history_document_state_descriptor_from_ui_process(*document_state, document_state_descriptor);
    return document_state;
}

NonnullRefPtr<SessionHistoryEntry> create_session_history_entry_from_ui_process(SessionHistoryEntryDescriptor entry_descriptor, SessionHistoryEntryReconstructionState& reconstruction_state)
{
    auto& directive_state = reconstruction_state.directive_states.ensure(entry_descriptor.directive_state_id, [&] {
        return DirectiveState::create(entry_descriptor.directive_state_id, entry_descriptor.directive_state_value);
    });
    directive_state->set_value(entry_descriptor.directive_state_value);
    auto entry = SessionHistoryEntry::create(directive_state);
    apply_session_history_entry_descriptor_from_ui_process(*entry, entry_descriptor);

    auto document_state = get_or_create_document_state_from_ui_process(entry_descriptor.document_state, reconstruction_state);
    VERIFY(document_state);
    entry->set_document_state(move(document_state));
    return entry;
}

}
