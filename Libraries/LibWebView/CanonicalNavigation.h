/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
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
    // Admitted, before navigate runs its steps in parallel.
    struct Admitted { };
    // Navigating to a javascript: URL, in the page with the document it is evaluated against.
    struct EvaluatingJavaScriptURL {
        NonnullRefPtr<WebContentPage> host;
    };
    // Navigate, step 23.1: checking if unloading is canceled, in the page with the source document.
    struct CheckingIfUnloadingIsCanceled {
        NonnullRefPtr<WebContentPage> worker;
        Web::HTML::NavigationStartRequest start_request;
    };
    // Attempting to populate the history entry's document, steps 1 to 4, in the page with the source document.
    struct CreatingNavigationParams {
        NonnullRefPtr<WebContentPage> worker;
        NonnullOwnPtr<NavigationLoader> loader;
    };
    // Acquiring the response body, in the page with the source document.
    struct AcquiringResponseBody {
        NonnullRefPtr<WebContentPage> worker;
        NonnullOwnPtr<NavigationLoader> loader;
    };
    // Choosing the page to host the document the response creates.
    struct ChoosingHost {
        NonnullRefPtr<WebContentPage> worker;
        NonnullOwnPtr<NavigationLoader> loader;
    };
    // Attempting to populate the history entry's document, step 5 on, in the page to host it.
    struct Populating {
        NonnullRefPtr<WebContentPage> host;
        NonnullOwnPtr<NavigationLoader> loader;
    };
    using State = Variant<Admitted, EvaluatingJavaScriptURL, CheckingIfUnloadingIsCanceled, CreatingNavigationParams, AcquiringResponseBody, ChoosingHost, Populating>;

    Optional<URL::URL> url {};
    Optional<Utf16String> navigation_id {};
    // The navigation to make again if the user stops this one and reloads, or a crash cuts it short. Kept from the start
    // request, which population uses up. A javascript: URL has none.
    Optional<Web::HTML::PreparedNavigationDescriptor> retry {};
    u64 sequence_number { 0 };
    State state { Admitted {} };
    Optional<PopulatedDocument> populated_document {};

    WebContentPage* worker() const;
    WebContentPage* host() const;
    NavigationLoader* loader() const;
};

WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::PreparedNavigationDescriptor);
WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::NavigationStartRequest const&);

}
