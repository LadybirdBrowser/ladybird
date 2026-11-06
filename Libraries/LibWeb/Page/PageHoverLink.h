/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Forward.h>

namespace Web {

// The link a page asks its client to show the URL of, as the pointer hovers it. No script takes part in this, so what
// is hovered may be told from any thread: the main thread tells as it handles the pointer, and a hover of the render
// clock tells while a task runs, on the Render thread. Each asks under one lock, so the client is left showing the link
// asked for last.
class WEB_API PageHoverLink final : public AtomicRefCounted<PageHoverLink> {
public:
    // Asks the client to show the URL of a link, or none, from any thread.
    using Send = Function<void(Optional<URL::URL> const&)>;

    static NonnullRefPtr<PageHoverLink> create() { return adopt_ref(*new PageHoverLink); }

    Optional<URL::URL> current() const;
    bool has_link() const;

    // Asks the client to show `url`, or no link, where the link asked for last is another. `document` is the document
    // the pointer is over, if any.
    void request(Optional<URL::URL> url, Optional<UniqueNodeID> document);

    // Asks the client to show no link, where what it shows was asked for over the document `document`.
    void withdraw(UniqueNodeID document);

    // What asks the client from now on. None asks nothing.
    void set_send(Send);

private:
    PageHoverLink() = default;

    void update(Optional<URL::URL> url);

    mutable Mutex m_mutex;
    Optional<URL::URL> m_current;
    Optional<UniqueNodeID> m_document;
    Send m_send;
};

// What tells a page the link a hover of the render clock is over, in one document. A hover cannot read the document,
// so the hrefs of its links are kept here by the style node of each link, with the base URL and encoding that resolve
// them.
class WEB_API HoverLinkReporter final : public AtomicRefCounted<HoverLinkReporter> {
public:
    static NonnullRefPtr<HoverLinkReporter> create(NonnullRefPtr<PageHoverLink>, UniqueNodeID document);

    bool resolves_as(String const& serialized_base_url, Utf16String const& encoding) const;
    void set_base(URL::URL base_url, Utf16String encoding);

    // Notes the href of the link `element` is, or that it is no link.
    void set_link(u32 element, Optional<Utf16String> href);

    // Whether `element` is a link. From any thread.
    bool is_link(u32 element) const;

    // Tells the page of the link `element` is, or of no link where it is none. From any thread.
    void report(u32 element);

    // Tells the page that the pointer left the document. From any thread.
    void withdraw();

    // Tells the page of no more links, as the document is being replaced. A hover of the render clock shows the frame on
    // screen, which may still be of this document after the client has heard that its hover ended.
    void retire();

private:
    HoverLinkReporter(NonnullRefPtr<PageHoverLink>, UniqueNodeID document);

    NonnullRefPtr<PageHoverLink> m_hover_link;
    UniqueNodeID m_document;

    mutable Mutex m_mutex;
    HashMap<u32, Utf16String> m_links;
    URL::URL m_base_url;
    Utf16String m_encoding;
    String m_encoding_name;
    bool m_is_retired { false };
};

}
