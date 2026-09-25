/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/NavigationPopulationRequest.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

struct PopulatedDocument {
    NonnullRefPtr<CanonicalDocumentState> document_state;
    NonnullRefPtr<CanonicalDocument> document;
};

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#ongoing-navigation
// A navigable's navigation, from its admission until its document is activated or it is canceled or superseded, with
// what it acquires on the way.
class CanonicalNavigation {
public:
    enum class Phase : u8 {
        Started,
        AwaitingUnloadCheck,
        Populating,
        AwaitingResponseBody,
    };

    Optional<URL::URL> url {};
    Optional<Utf16String> navigation_id {};
    Optional<Web::HTML::NavigationStartRequest> start_request {};
    u64 sequence_number { 0 };
    bool has_started { false };
    Phase phase { Phase::Started };
    OwnPtr<NavigationLoader> loader {};
    RefPtr<WebContentPage> population_worker {};
    RefPtr<WebContentPage> host {};
    // AD-HOC: The session history entry whose document a navigation reconstructing a child navigable's history
    //         populates.
    RefPtr<CanonicalSessionHistoryEntry> reconstructed_entry {};
    Optional<PopulatedDocument> populated_document {};
};

}
