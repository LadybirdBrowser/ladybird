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
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

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
    , m_navigation_api_key(Crypto::generate_random_uuid())
    , m_navigation_api_id(Crypto::generate_random_uuid())
{
}

static u64 document_state_id_for_descriptor(DocumentState const& document_state, SessionHistoryEntryDescriptorCreationState& creation_state)
{
    if (auto id = creation_state.document_state_ids.get(&document_state); id.has_value())
        return *id;

    auto id = creation_state.next_document_state_id++;
    VERIFY(id != 0);
    creation_state.document_state_ids.set(&document_state, id);
    return id;
}

static SessionHistoryDocumentStateDescriptor create_session_history_document_state_descriptor(DocumentState const& document_state, SessionHistoryEntryDescriptorCreationState& creation_state)
{
    Vector<SessionHistoryNestedHistoryDescriptor> nested_history_descriptors;
    nested_history_descriptors.ensure_capacity(document_state.nested_histories().size());
    for (auto const& nested_history : document_state.nested_histories()) {
        Vector<SessionHistoryEntryDescriptor> nested_entry_descriptors;
        nested_entry_descriptors.ensure_capacity(nested_history.entries.size());
        for (auto const& nested_entry : nested_history.entries) {
            // NB: UI-process session history mirrors only concrete used history steps. A child entry whose step is
            //     still "pending" has not been attached to the traversable's step graph yet.
            if (!nested_entry->step_value().has_value())
                continue;
            nested_entry_descriptors.unchecked_append(create_session_history_entry_descriptor(nested_entry, creation_state));
        }

        // NB: Keep the nested-history descriptor even when every entry in it is still pending. The entries are not
        //     used history steps yet, but the descriptor id preserves the live child navigable identity when the UI
        //     process later reseeds an already-loaded document.
        nested_history_descriptors.unchecked_append({
            .id = nested_history.id,
            .entries = move(nested_entry_descriptors),
        });
    }

    return {
        .id = document_state_id_for_descriptor(document_state, creation_state),
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
        .nested_histories = move(nested_history_descriptors),
    };
}

SessionHistoryEntryDescriptor create_session_history_entry_descriptor(SessionHistoryEntry const& entry, SessionHistoryEntryDescriptorCreationState& creation_state)
{
    auto entry_step = entry.step_value();
    VERIFY(entry_step.has_value());
    SessionHistoryDocumentStateDescriptor document_state_descriptor;
    if (auto document_state = entry.document_state())
        document_state_descriptor = create_session_history_document_state_descriptor(*document_state, creation_state);

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

static bool session_history_nested_history_descriptors_match(Vector<SessionHistoryNestedHistoryDescriptor> const& a, Vector<SessionHistoryNestedHistoryDescriptor> const& b);

static bool serialized_directives_match(Vector<ContentSecurityPolicy::Directives::SerializedDirective> const& a, Vector<ContentSecurityPolicy::Directives::SerializedDirective> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].name != b[i].name || a[i].value != b[i].value)
            return false;
    }
    return true;
}

static bool serialized_policies_match(Vector<ContentSecurityPolicy::SerializedPolicy> const& a, Vector<ContentSecurityPolicy::SerializedPolicy> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (!serialized_directives_match(a[i].directives, b[i].directives)
            || a[i].disposition != b[i].disposition
            || a[i].source != b[i].source
            || a[i].self_origin != b[i].self_origin
            || a[i].pre_parsed_policy_string != b[i].pre_parsed_policy_string)
            return false;
    }
    return true;
}

static bool embedder_policies_match(EmbedderPolicy const& a, EmbedderPolicy const& b)
{
    return a.value == b.value
        && a.report_only_value == b.report_only_value
        && a.reporting_endpoint == b.reporting_endpoint
        && a.report_only_reporting_endpoint == b.report_only_reporting_endpoint;
}

static bool serialized_policy_containers_match(SerializedPolicyContainer const& a, SerializedPolicyContainer const& b)
{
    return serialized_policies_match(a.csp_list, b.csp_list)
        && embedder_policies_match(a.embedder_policy, b.embedder_policy)
        && a.referrer_policy == b.referrer_policy;
}

static bool history_policy_containers_match(Variant<SerializedPolicyContainer, DocumentState::Client> const& a, Variant<SerializedPolicyContainer, DocumentState::Client> const& b)
{
    if (auto const* a_serialized_policy_container = a.get_pointer<SerializedPolicyContainer>()) {
        auto const* b_serialized_policy_container = b.get_pointer<SerializedPolicyContainer>();
        return b_serialized_policy_container && serialized_policy_containers_match(*a_serialized_policy_container, *b_serialized_policy_container);
    }

    return a.has<DocumentState::Client>() && b.has<DocumentState::Client>();
}

static bool session_history_document_state_descriptors_match(SessionHistoryDocumentStateDescriptor const& a, SessionHistoryDocumentStateDescriptor const& b)
{
    return a.id == b.id
        && history_policy_containers_match(a.history_policy_container, b.history_policy_container)
        && a.request_referrer == b.request_referrer
        && a.request_referrer_policy == b.request_referrer_policy
        && a.initiator_origin == b.initiator_origin
        && a.origin == b.origin
        && a.about_base_url == b.about_base_url
        && a.resource == b.resource
        && a.reload_pending == b.reload_pending
        && a.ever_populated == b.ever_populated
        && a.navigable_target_name == b.navigable_target_name
        && session_history_nested_history_descriptors_match(a.nested_histories, b.nested_histories);
}

static bool session_history_nested_history_descriptors_match_ignoring_document_state_ids(Vector<SessionHistoryNestedHistoryDescriptor> const&, Vector<SessionHistoryNestedHistoryDescriptor> const&);

static bool session_history_document_state_descriptors_match_ignoring_id(SessionHistoryDocumentStateDescriptor const& a, SessionHistoryDocumentStateDescriptor const& b, MatchNestedHistories match_nested_histories)
{
    if (!(history_policy_containers_match(a.history_policy_container, b.history_policy_container)
            && a.request_referrer == b.request_referrer
            && a.request_referrer_policy == b.request_referrer_policy
            && a.initiator_origin == b.initiator_origin
            && a.origin == b.origin
            && a.about_base_url == b.about_base_url
            && a.resource == b.resource
            && a.reload_pending == b.reload_pending
            && a.ever_populated == b.ever_populated
            && a.navigable_target_name == b.navigable_target_name))
        return false;

    if (match_nested_histories == MatchNestedHistories::No)
        return true;

    return session_history_nested_history_descriptors_match_ignoring_document_state_ids(a.nested_histories, b.nested_histories);
}

bool session_history_entry_descriptors_match(SessionHistoryEntryDescriptor const& a, SessionHistoryEntryDescriptor const& b)
{
    return a.step == b.step
        && a.url == b.url
        && session_history_document_state_descriptors_match(a.document_state, b.document_state)
        && a.classic_history_api_state == b.classic_history_api_state
        && a.navigation_api_state == b.navigation_api_state
        && a.navigation_api_key == b.navigation_api_key
        && a.navigation_api_id == b.navigation_api_id
        && a.scroll_restoration_mode == b.scroll_restoration_mode
        && a.scroll_position_data == b.scroll_position_data;
}

bool session_history_entry_descriptors_match_ignoring_document_state_id(SessionHistoryEntryDescriptor const& a, SessionHistoryEntryDescriptor const& b, MatchNestedHistories match_nested_histories)
{
    if (a.step != b.step)
        return false;
    if (a.url != b.url)
        return false;
    if (a.document_state.id != 0 && !session_history_document_state_descriptors_match_ignoring_id(a.document_state, b.document_state, match_nested_histories))
        return false;
    if (a.classic_history_api_state != b.classic_history_api_state)
        return false;
    if (a.navigation_api_state != b.navigation_api_state || a.navigation_api_key != b.navigation_api_key || a.navigation_api_id != b.navigation_api_id)
        return false;
    if (a.scroll_restoration_mode != b.scroll_restoration_mode)
        return false;
    if (a.document_state.id != 0 && a.scroll_position_data != b.scroll_position_data)
        return false;
    return true;
}

static bool session_history_entry_descriptors_match(Vector<SessionHistoryEntryDescriptor> const& a, Vector<SessionHistoryEntryDescriptor> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (session_history_entry_descriptors_match(a[i], b[i]))
            continue;
        return false;
    }
    return true;
}

static bool session_history_entry_descriptors_match_ignoring_document_state_ids(Vector<SessionHistoryEntryDescriptor> const& a, Vector<SessionHistoryEntryDescriptor> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (session_history_entry_descriptors_match_ignoring_document_state_id(a[i], b[i]))
            continue;
        return false;
    }
    return true;
}

static bool session_history_nested_history_descriptors_match(Vector<SessionHistoryNestedHistoryDescriptor> const& a, Vector<SessionHistoryNestedHistoryDescriptor> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id == b[i].id && session_history_entry_descriptors_match(a[i].entries, b[i].entries))
            continue;
        return false;
    }
    return true;
}

static bool session_history_nested_history_descriptors_match_ignoring_document_state_ids(Vector<SessionHistoryNestedHistoryDescriptor> const& a, Vector<SessionHistoryNestedHistoryDescriptor> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].id == b[i].id && session_history_entry_descriptors_match_ignoring_document_state_ids(a[i].entries, b[i].entries))
            continue;
        return false;
    }
    return true;
}

static bool session_history_nested_history_descriptors_match_document_state_nested_histories(Vector<SessionHistoryNestedHistoryDescriptor> const& descriptors, Vector<DocumentState::NestedHistory> const& nested_histories)
{
    if (descriptors.size() != nested_histories.size())
        return false;

    for (size_t i = 0; i < descriptors.size(); ++i) {
        auto const& descriptor = descriptors[i];
        auto const& nested_history = nested_histories[i];
        if (descriptor.id != nested_history.id || descriptor.entries.size() != nested_history.entries.size())
            return false;
        for (size_t j = 0; j < descriptor.entries.size(); ++j) {
            if (!session_history_entry_matches_descriptor_ignoring_document_state_id(*nested_history.entries[j], descriptor.entries[j]))
                return false;
        }
    }

    return true;
}

static bool session_history_document_state_descriptor_matches_document_state_ignoring_id(SessionHistoryDocumentStateDescriptor const& descriptor, DocumentState const& document_state, MatchNestedHistories match_nested_histories)
{
    if (!(history_policy_containers_match(descriptor.history_policy_container, document_state.history_policy_container())
            && descriptor.request_referrer == document_state.request_referrer()
            && descriptor.request_referrer_policy == document_state.request_referrer_policy()
            && descriptor.initiator_origin == document_state.initiator_origin()
            && descriptor.origin == document_state.origin()
            && descriptor.about_base_url == document_state.about_base_url()
            && descriptor.resource == document_state.resource()
            && descriptor.reload_pending == document_state.reload_pending()
            && descriptor.ever_populated == document_state.ever_populated()
            && descriptor.navigable_target_name == document_state.navigable_target_name()))
        return false;

    if (match_nested_histories == MatchNestedHistories::No)
        return true;

    return session_history_nested_history_descriptors_match_document_state_nested_histories(descriptor.nested_histories, document_state.nested_histories());
}

bool session_history_entry_matches_descriptor_ignoring_document_state_id(SessionHistoryEntry const& entry, SessionHistoryEntryDescriptor const& descriptor, MatchNestedHistories match_nested_histories)
{
    auto entry_step = entry.step_value();
    if (!entry_step.has_value() || *entry_step != descriptor.step)
        return false;
    if (entry.url() != descriptor.url)
        return false;
    if (descriptor.document_state.id != 0) {
        auto document_state = entry.document_state();
        if (!document_state || !session_history_document_state_descriptor_matches_document_state_ignoring_id(descriptor.document_state, *document_state, match_nested_histories))
            return false;
    }
    if (entry.classic_history_api_state() != descriptor.classic_history_api_state)
        return false;
    if (entry.navigation_api_state() != descriptor.navigation_api_state || entry.navigation_api_key() != descriptor.navigation_api_key || entry.navigation_api_id() != descriptor.navigation_api_id)
        return false;
    if (entry.scroll_restoration_mode() != descriptor.scroll_restoration_mode)
        return false;
    if (descriptor.document_state.id != 0 && entry.scroll_position_data() != descriptor.scroll_position_data)
        return false;
    return true;
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
    auto classic_history_api_state = TRY(decoder.decode<Web::HTML::SerializationRecord>());
    auto navigation_api_state = TRY(decoder.decode<Web::HTML::SerializationRecord>());
    auto navigation_api_key = TRY(decoder.decode<String>());
    auto navigation_api_id = TRY(decoder.decode<String>());
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
    auto id = TRY(decoder.decode<u64>());
    auto history_policy_container = TRY(decoder.decode<Variant<Web::HTML::SerializedPolicyContainer, Web::HTML::DocumentState::Client>>());
    auto request_referrer = TRY(decoder.decode<Web::Fetch::Infrastructure::Request::ReferrerType>());
    auto request_referrer_policy = TRY(decoder.decode<Web::ReferrerPolicy::ReferrerPolicy>());
    auto initiator_origin = TRY(decoder.decode<Optional<URL::Origin>>());
    auto origin = TRY(decoder.decode<Optional<URL::Origin>>());
    auto about_base_url = TRY(decoder.decode<Optional<URL::URL>>());
    auto resource = TRY(decoder.decode<Variant<Empty, String, Web::HTML::POSTResource>>());
    auto reload_pending = TRY(decoder.decode<bool>());
    auto ever_populated = TRY(decoder.decode<bool>());
    auto navigable_target_name = TRY(decoder.decode<String>());
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
    auto id = TRY(decoder.decode<String>());
    auto entries = TRY(decoder.decode<Vector<Web::HTML::SessionHistoryEntryDescriptor>>());

    return Web::HTML::SessionHistoryNestedHistoryDescriptor { move(id), move(entries) };
}
