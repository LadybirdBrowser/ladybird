/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/history.html#scroll-restoration-mode
enum class ScrollRestorationMode {
    // https://html.spec.whatwg.org/multipage/history.html#dom-scrollrestoration-auto
    // The user agent is responsible for restoring the scroll position upon navigation.
    Auto,

    // https://html.spec.whatwg.org/multipage/history.html#dom-scrollrestoration-manual
    // The page is responsible for restoring the scroll position and the user agent does not attempt to do so automatically.
    Manual,
};

enum class SessionHistoryEntryUpdateKind : u8 {
    NavigationAPIState,
    ScrollRestorationMode,
    ScrollPositionData,
    DocumentStateReloadPending,
    DocumentStatePopulation,
};

struct SessionHistoryNestedHistoryDescriptor;

// IPC-friendly descriptors for the parts of session history entries and document states that can survive
// WebContent process swaps.
//
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history-entry
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#document-state
struct SessionHistoryDocumentStateDescriptor {
    // AD-HOC: The spec models shared document state by object identity. The UI-process mirror uses a stable
    //         descriptor ID so entries that share a document state can be reconstructed after IPC.
    CrossProcessId id;
    Variant<SerializedPolicyContainer, DocumentState::Client> history_policy_container { DocumentState::Client::Tag };
    Fetch::Infrastructure::Request::ReferrerType request_referrer { Fetch::Infrastructure::Request::Referrer::Client };
    ReferrerPolicy::ReferrerPolicy request_referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };
    Optional<URL::Origin> initiator_origin;
    Optional<URL::Origin> origin;
    Optional<URL::URL> about_base_url;
    DocumentResource resource;
    bool reload_pending { false };
    bool ever_populated { false };
    bool is_provisional { false };
    Utf16String navigable_target_name;
    Vector<SessionHistoryNestedHistoryDescriptor> nested_histories;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-position
struct SessionHistoryEntryScrollPositionData {
    // FIXME: Track all restorable scrollable regions. Currently only the viewport is persisted.
    Optional<CSSPixelPoint> viewport_scroll_position;

    bool operator==(SessionHistoryEntryScrollPositionData const&) const = default;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history-entry
struct SessionHistoryEntryDescriptor {
    i32 step { 0 };
    URL::URL url;
    SessionHistoryDocumentStateDescriptor document_state;
    StorageSerializationRecord classic_history_api_state;
    StorageSerializationRecord navigation_api_state;
    Utf16String navigation_api_key;
    Utf16String navigation_api_id;
    ScrollRestorationMode scroll_restoration_mode { ScrollRestorationMode::Auto };
    SessionHistoryEntryScrollPositionData scroll_position_data;
};

struct SameDocumentSessionHistoryNavigation {
    SessionHistoryEntryDescriptor entry;
    Optional<i32> replaced_step;
    i32 current_step { 0 };
};

struct NestedSameDocumentSessionHistoryNavigation {
    CrossProcessId parent_document_state_id;
    CrossProcessId navigable_id;
    SessionHistoryEntryDescriptor entry;
    Optional<i32> replaced_step;
    i32 current_step { 0 };
};

struct NestedCrossDocumentSessionHistoryNavigation {
    CrossProcessId parent_document_state_id;
    CrossProcessId navigable_id;
    SessionHistoryEntryDescriptor entry;
    i32 current_step { 0 };
};

struct TopLevelCrossDocumentSessionHistoryNavigation {
    SessionHistoryEntryDescriptor entry;
    i32 current_step { 0 };
};

struct AppliedSessionHistoryTraversal {
    i32 current_step { 0 };
};

struct RestoredCurrentSessionHistoryStep {
    i32 current_step { 0 };
};

struct CurrentSessionHistoryEntryUpdate {
    SessionHistoryEntryUpdateKind update_kind { SessionHistoryEntryUpdateKind::NavigationAPIState };
    SessionHistoryEntryDescriptor entry;
};

struct CurrentSessionHistoryEntryNestedHistoriesUpdate {
    CrossProcessId document_state_id;
    Vector<SessionHistoryNestedHistoryDescriptor> nested_histories;
    i32 current_step { 0 };
};

struct WebContentSessionHistoryMutation {
    using Mutation = Variant<CurrentSessionHistoryEntryUpdate, CurrentSessionHistoryEntryNestedHistoriesUpdate, SameDocumentSessionHistoryNavigation, NestedSameDocumentSessionHistoryNavigation, NestedCrossDocumentSessionHistoryNavigation, TopLevelCrossDocumentSessionHistoryNavigation, AppliedSessionHistoryTraversal, RestoredCurrentSessionHistoryStep>;

    Mutation mutation;

    static WebContentSessionHistoryMutation current_entry_update(SessionHistoryEntryUpdateKind update_kind, SessionHistoryEntryDescriptor entry)
    {
        return { CurrentSessionHistoryEntryUpdate { update_kind, move(entry) } };
    }

    static WebContentSessionHistoryMutation current_entry_nested_histories_update(CurrentSessionHistoryEntryNestedHistoriesUpdate update)
    {
        return { move(update) };
    }

    static WebContentSessionHistoryMutation top_level_same_document_navigation(SameDocumentSessionHistoryNavigation navigation)
    {
        return { move(navigation) };
    }

    static WebContentSessionHistoryMutation nested_same_document_navigation(NestedSameDocumentSessionHistoryNavigation navigation)
    {
        return { move(navigation) };
    }

    static WebContentSessionHistoryMutation nested_cross_document_navigation(NestedCrossDocumentSessionHistoryNavigation navigation)
    {
        return { move(navigation) };
    }

    static WebContentSessionHistoryMutation top_level_cross_document_navigation(TopLevelCrossDocumentSessionHistoryNavigation navigation)
    {
        return { move(navigation) };
    }

    static WebContentSessionHistoryMutation applied_traversal(AppliedSessionHistoryTraversal traversal)
    {
        return { traversal };
    }

    static WebContentSessionHistoryMutation restored_current_step(RestoredCurrentSessionHistoryStep step)
    {
        return { step };
    }
};

struct WebContentSessionHistoryMutationBatch {
    Vector<WebContentSessionHistoryMutation> mutations;
    i32 final_current_step { 0 };
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#nested-history
struct SessionHistoryNestedHistoryDescriptor {
    CrossProcessId id;
    Vector<SessionHistoryEntryDescriptor> entries;
};

// https://html.spec.whatwg.org/multipage/history.html#session-history-entry
class WEB_API SessionHistoryEntry final : public RefCounted<SessionHistoryEntry> {
public:
    static NonnullRefPtr<SessionHistoryEntry> create();

    SessionHistoryEntry();
    ~SessionHistoryEntry();

    enum class Pending {
        Tag,
    };

    [[nodiscard]] Variant<int, Pending> step() const { return m_step; }
    void set_step(Variant<int, Pending> step) { m_step = step; }
    [[nodiscard]] Optional<int> step_value() const
    {
        if (auto const* step = m_step.get_pointer<int>())
            return *step;
        return {};
    }

    [[nodiscard]] URL::URL const& url() const { return m_url; }
    void set_url(URL::URL url) { m_url = move(url); }

    [[nodiscard]] RefPtr<HTML::DocumentState> document_state() const;
    void set_document_state(RefPtr<HTML::DocumentState>);

    [[nodiscard]] StorageSerializationRecord const& classic_history_api_state() const { return m_classic_history_api_state; }
    void set_classic_history_api_state(StorageSerializationRecord classic_history_api_state) { m_classic_history_api_state = move(classic_history_api_state); }

    [[nodiscard]] StorageSerializationRecord const& navigation_api_state() const { return m_navigation_api_state; }
    void set_navigation_api_state(StorageSerializationRecord navigation_api_state) { m_navigation_api_state = move(navigation_api_state); }

    [[nodiscard]] Utf16String const& navigation_api_key() const { return m_navigation_api_key; }
    void set_navigation_api_key(Utf16String navigation_api_key) { m_navigation_api_key = move(navigation_api_key); }

    [[nodiscard]] Utf16String const& navigation_api_id() const { return m_navigation_api_id; }
    void set_navigation_api_id(Utf16String navigation_api_id) { m_navigation_api_id = move(navigation_api_id); }

    [[nodiscard]] ScrollRestorationMode scroll_restoration_mode() const { return m_scroll_restoration_mode; }
    void set_scroll_restoration_mode(ScrollRestorationMode scroll_restoration_mode) { m_scroll_restoration_mode = scroll_restoration_mode; }

    [[nodiscard]] SessionHistoryEntryScrollPositionData const& scroll_position_data() const { return m_scroll_position_data; }
    void set_scroll_position_data(SessionHistoryEntryScrollPositionData scroll_position_data) { m_scroll_position_data = move(scroll_position_data); }

private:
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-step
    // step, a non-negative integer or "pending", initially "pending".
    Variant<int, Pending> m_step { Pending::Tag };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-url
    // URL, a URL
    URL::URL m_url;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-document-state
    RefPtr<HTML::DocumentState> m_document_state;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-classic-history-api-state
    // classic history API state, which is serialized state, initially StructuredSerializeForStorage(null).
    StorageSerializationRecord m_classic_history_api_state;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-navigation-api-state
    // navigation API state, which is a serialized state, initially StructuredSerializeForStorage(undefined).
    StorageSerializationRecord m_navigation_api_state;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-navigation-api-key
    // navigation API key, which is a string, initially set to the result of generating a random UUID.
    Utf16String m_navigation_api_key;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-navigation-api-id
    // navigation API ID, which is a string, initially set to the result of generating a random UUID.
    Utf16String m_navigation_api_id;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-restoration-mode
    // scroll restoration mode, a scroll restoration mode, initially "auto"
    ScrollRestorationMode m_scroll_restoration_mode { ScrollRestorationMode::Auto };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-position
    // scroll position data, which is scroll position data for the document's restorable scrollable regions
    SessionHistoryEntryScrollPositionData m_scroll_position_data;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-other
    // FIXME: persisted user state, which is implementation-defined, initially null
    // NOTE: This is where we could remember the state of form controls, for example.
};

struct SessionHistoryEntryDescriptorCreationState {
    explicit SessionHistoryEntryDescriptorCreationState(Function<CrossProcessId()> allocate_cross_process_id)
        : allocate_cross_process_id(move(allocate_cross_process_id))
    {
    }

    Function<CrossProcessId()> allocate_cross_process_id;
};

WEB_API SessionHistoryEntryDescriptor create_session_history_entry_descriptor(SessionHistoryEntry const&, SessionHistoryEntryDescriptorCreationState&);
WEB_API bool session_history_entry_descriptors_match(SessionHistoryEntryDescriptor const&, SessionHistoryEntryDescriptor const&);
enum class MatchNestedHistories {
    Yes,
    No,
};
WEB_API bool session_history_entry_matches_descriptor_ignoring_document_state_id(SessionHistoryEntry const&, SessionHistoryEntryDescriptor const&, MatchNestedHistories = MatchNestedHistories::Yes);

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryEntryDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::SessionHistoryEntryDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::CurrentSessionHistoryEntryUpdate const&);

template<>
WEB_API ErrorOr<Web::HTML::CurrentSessionHistoryEntryUpdate> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::CurrentSessionHistoryEntryNestedHistoriesUpdate const&);

template<>
WEB_API ErrorOr<Web::HTML::CurrentSessionHistoryEntryNestedHistoriesUpdate> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SameDocumentSessionHistoryNavigation const&);

template<>
WEB_API ErrorOr<Web::HTML::SameDocumentSessionHistoryNavigation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NestedSameDocumentSessionHistoryNavigation const&);

template<>
WEB_API ErrorOr<Web::HTML::NestedSameDocumentSessionHistoryNavigation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::NestedCrossDocumentSessionHistoryNavigation const&);

template<>
WEB_API ErrorOr<Web::HTML::NestedCrossDocumentSessionHistoryNavigation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::TopLevelCrossDocumentSessionHistoryNavigation const&);

template<>
WEB_API ErrorOr<Web::HTML::TopLevelCrossDocumentSessionHistoryNavigation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::AppliedSessionHistoryTraversal const&);

template<>
WEB_API ErrorOr<Web::HTML::AppliedSessionHistoryTraversal> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::RestoredCurrentSessionHistoryStep const&);

template<>
WEB_API ErrorOr<Web::HTML::RestoredCurrentSessionHistoryStep> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::WebContentSessionHistoryMutation const&);

template<>
WEB_API ErrorOr<Web::HTML::WebContentSessionHistoryMutation> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::WebContentSessionHistoryMutationBatch const&);

template<>
WEB_API ErrorOr<Web::HTML::WebContentSessionHistoryMutationBatch> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryEntryScrollPositionData const&);

template<>
WEB_API ErrorOr<Web::HTML::SessionHistoryEntryScrollPositionData> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryDocumentStateDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::SessionHistoryDocumentStateDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::HTML::SessionHistoryNestedHistoryDescriptor const&);

template<>
WEB_API ErrorOr<Web::HTML::SessionHistoryNestedHistoryDescriptor> decode(Decoder&);

}
