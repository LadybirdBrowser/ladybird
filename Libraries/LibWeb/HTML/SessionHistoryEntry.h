/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/StructuredSerialize.h>

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

// https://html.spec.whatwg.org/multipage/history.html#session-history-entry
class SessionHistoryEntry final : public JS::Cell {
    GC_CELL(SessionHistoryEntry, JS::Cell);
    GC_DECLARE_ALLOCATOR(SessionHistoryEntry);

public:
    SessionHistoryEntry();

    GC::Ref<SessionHistoryEntry> clone() const;

    void visit_edges(Cell::Visitor&) override;

    GC::Ptr<DOM::Document> document() const;

    enum class Pending {
        Tag,
    };

    [[nodiscard]] Variant<int, Pending> step() const { return m_step; }
    void set_step(Variant<int, Pending> step) { m_step = step; }

    [[nodiscard]] URL::URL const& url() const { return m_url; }
    void set_url(URL::URL url) { m_url = move(url); }

    [[nodiscard]] GC::Ptr<HTML::DocumentState> document_state() const { return m_document_state; }
    void set_document_state(GC::Ptr<HTML::DocumentState> document_state) { m_document_state = document_state; }

    [[nodiscard]] SerializationRecord const& classic_history_api_state() const { return m_classic_history_api_state; }
    void set_classic_history_api_state(SerializationRecord classic_history_api_state) { m_classic_history_api_state = move(classic_history_api_state); }

    [[nodiscard]] SerializationRecord const& navigation_api_state() const { return m_navigation_api_state; }
    void set_navigation_api_state(SerializationRecord navigation_api_state) { m_navigation_api_state = move(navigation_api_state); }

    [[nodiscard]] String const& navigation_api_key() const { return m_navigation_api_key; }
    void set_navigation_api_key(String navigation_api_key) { m_navigation_api_key = move(navigation_api_key); }

    [[nodiscard]] String const& navigation_api_id() const { return m_navigation_api_id; }
    void set_navigation_api_id(String navigation_api_id) { m_navigation_api_id = move(navigation_api_id); }

    [[nodiscard]] ScrollRestorationMode scroll_restoration_mode() const { return m_scroll_restoration_mode; }
    void set_scroll_restoration_mode(ScrollRestorationMode scroll_restoration_mode) { m_scroll_restoration_mode = scroll_restoration_mode; }

    [[nodiscard]] GC::Ptr<PolicyContainer> policy_container() const { return m_policy_container; }
    void set_policy_container(GC::Ptr<PolicyContainer> policy_container) { m_policy_container = policy_container; }

    [[nodiscard]] Optional<ByteString> const& browsing_context_name() const { return m_browsing_context_name; }
    void set_browsing_context_name(Optional<ByteString> browsing_context_name) { m_browsing_context_name = move(browsing_context_name); }

    [[nodiscard]] GC::Ptr<BrowsingContext> original_source_browsing_context() const { return m_original_source_browsing_context; }
    void set_original_source_browsing_context(GC::Ptr<BrowsingContext> original_source_browsing_context) { m_original_source_browsing_context = original_source_browsing_context; }

private:
    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-step
    // step, a non-negative integer or "pending", initially "pending".
    Variant<int, Pending> m_step { Pending::Tag };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-url
    // URL, a URL
    URL::URL m_url;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-document-state
    GC::Ptr<HTML::DocumentState> m_document_state;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-classic-history-api-state
    // classic history API state, which is serialized state, initially StructuredSerializeForStorage(null).
    SerializationRecord m_classic_history_api_state;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-navigation-api-state
    // navigation API state, which is a serialized state, initially StructuredSerializeForStorage(undefined).
    SerializationRecord m_navigation_api_state;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-navigation-api-key
    // navigation API key, which is a string, initially set to the result of generating a random UUID.
    String m_navigation_api_key;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-navigation-api-id
    // navigation API ID, which is a string, initially set to the result of generating a random UUID.
    String m_navigation_api_id;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-restoration-mode
    // scroll restoration mode, a scroll restoration mode, initially "auto"
    ScrollRestorationMode m_scroll_restoration_mode { ScrollRestorationMode::Auto };

    // policy container, a policy container or null
    GC::Ptr<PolicyContainer> m_policy_container;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-position
    // FIXME: scroll position data, which is scroll position data for the document's restorable scrollable regions

    // browsing context name, a browsing context name or null, initially null
    Optional<ByteString> m_browsing_context_name;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-other
    // FIXME: persisted user state, which is implementation-defined, initially null
    // NOTE: This is where we could remember the state of form controls, for example.

    GC::Ptr<BrowsingContext> m_original_source_browsing_context;
};

}
