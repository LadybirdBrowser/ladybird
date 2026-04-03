/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>

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
class SessionHistoryEntry final : public RefCounted<SessionHistoryEntry> {
public:
    static NonnullRefPtr<SessionHistoryEntry> create();

    SessionHistoryEntry();
    ~SessionHistoryEntry();

    enum class Pending {
        Tag,
    };

    [[nodiscard]] Variant<int, Pending> step() const { return m_step; }
    void set_step(Variant<int, Pending> step) { m_step = step; }

    [[nodiscard]] URL::URL const& url() const { return m_url; }
    void set_url(URL::URL url) { m_url = move(url); }

    [[nodiscard]] RefPtr<HTML::DocumentState> document_state() const;
    void set_document_state(RefPtr<HTML::DocumentState>);

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

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-scroll-position
    // FIXME: scroll position data, which is scroll position data for the document's restorable scrollable regions

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-other
    // FIXME: persisted user state, which is implementation-defined, initially null
    // NOTE: This is where we could remember the state of form controls, for example.
};

}
