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
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

struct PopulatedDocument {
    NonnullRefPtr<CanonicalDocumentState> document_state;
    NonnullRefPtr<CanonicalDocument> document;
    // The origin of a document the hosting process creates for inline content in place of the response.
    Optional<URL::Origin> inline_content_origin {};
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
    // The navigation to make again if the user stops this one and reloads, or a crash cuts it short. Kept from the start
    // request, which population uses up. A javascript: URL has none.
    Optional<Web::HTML::PreparedNavigationDescriptor> retry {};
    u64 sequence_number { 0 };
    Phase phase { Phase::Started };
    OwnPtr<NavigationLoader> loader {};
    RefPtr<WebContentPage> population_worker {};
    RefPtr<WebContentPage> host {};
    Optional<PopulatedDocument> populated_document {};

    bool is_conducted_by(WebContentPage const&) const;
};

WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::PreparedNavigationDescriptor);
WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::NavigationStartRequest const&);

}
