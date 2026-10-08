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

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
// The steps navigate runs for one navigation, from its admission until its document is activated or it ends without
// one. They go on after the navigable's ongoing navigation stops being their navigation ID only once their
// finalization is appended, or while a javascript: URL is evaluated.
class CanonicalNavigation {
    AK_ALLOC_WITH_KMALLOC;

public:
    // Navigating to a javascript: URL, in the page with the document it is evaluated against.
    struct EvaluatingJavaScriptURL { };
    // Navigate, step 23.1: checking if unloading is canceled, in the page with the source document.
    struct CheckingIfUnloadingIsCanceled {
        Web::HTML::NavigationStartRequest start_request;
    };
    // Attempting to populate the history entry's document, steps 1 to 4, in the page with the source document.
    struct CreatingNavigationParams {
        NonnullOwnPtr<NavigationLoader> loader;
    };
    // NB: The page with the source document hands the response body over to the page to host the document.
    struct AcquiringResponseBody {
        NonnullOwnPtr<NavigationLoader> loader;
    };
    // Attempting to populate the history entry's document, step 5 on, in the page to host it.
    struct Populating {
        NonnullOwnPtr<NavigationLoader> loader;
    };
    // Attempting to populate the history entry's document is over, and its completion steps appended finalizing the
    // navigation to the traversal queue.
    struct Finalizing {
        OwnPtr<NavigationLoader> loader;
    };
    using State = Variant<EvaluatingJavaScriptURL, CheckingIfUnloadingIsCanceled, CreatingNavigationParams, AcquiringResponseBody, Populating, Finalizing>;

    URL::URL url;
    Utf16String navigation_id;
    // The navigation to make again if the user stops this one and reloads, or a crash cuts it short. Kept from the start
    // request, which population uses up. A javascript: URL has none.
    Optional<Web::HTML::PreparedNavigationDescriptor> retry {};
    u64 sequence_number { 0 };
    State state;
    // The page running the navigation's steps.
    RefPtr<WebContentPage> page {};
    // Navigate's historyEntry, whose document state holds the document populated for it.
    RefPtr<CanonicalSessionHistoryEntry> history_entry {};

    WebContentPage* worker() const;
    WebContentPage* host() const;
    NavigationLoader* loader() const;
    bool is_finalizing() const { return state.has<Finalizing>(); }
};

WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::PreparedNavigationDescriptor);
WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::NavigationStartRequest const&);

}
