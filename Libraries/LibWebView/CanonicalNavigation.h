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
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <LibURL/URL.h>
#include <LibWebCommon/HTML/NavigationPopulationRequest.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/NavigationLoader.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
// One navigation, from its admission until its document is activated or it ends without one.
class CanonicalNavigation final : public RefCounted<CanonicalNavigation> {
    AK_ALLOC_WITH_KMALLOC;

public:
    // The worker is the page with the source document; the host is the page to host the new one.
    enum class RunsIn : u8 {
        Worker,
        Host,
    };

    // Step 20.
    struct EvaluatingJavaScriptURL {
        static constexpr auto runs_in = RunsIn::Host;
    };
    // Step 23.1.
    struct CheckingIfUnloadingIsCanceled {
        static constexpr auto runs_in = RunsIn::Worker;
        Web::HTML::NavigationStartRequest start_request;
    };
    // Attempt to populate the history entry's document, steps 1 to 4.
    struct CreatingNavigationParams {
        static constexpr auto runs_in = RunsIn::Worker;
        NonnullRefPtr<NavigationLoader> loader;
    };
    // NB: The worker hands the response body over to the host.
    struct AcquiringResponseBody {
        static constexpr auto runs_in = RunsIn::Worker;
        NonnullRefPtr<NavigationLoader> loader;
    };
    // Attempt to populate the history entry's document, step 5 on.
    struct Populating {
        static constexpr auto runs_in = RunsIn::Host;
        NonnullRefPtr<NavigationLoader> loader;
    };
    // The completion steps appended finalizing the navigation to the traversal queue.
    struct Finalizing {
        static constexpr auto runs_in = RunsIn::Host;
        RefPtr<NavigationLoader> loader;
    };
    using State = Variant<EvaluatingJavaScriptURL, CheckingIfUnloadingIsCanceled, CreatingNavigationParams, AcquiringResponseBody, Populating, Finalizing>;

    struct Start {
        URL::URL url;
        Utf16String navigation_id;
        Optional<Web::HTML::PreparedNavigationDescriptor> retry {};
        u64 sequence_number { 0 };
        State state;
        RefPtr<WebContentPage> page {};
        RefPtr<CanonicalSessionHistoryEntry> history_entry {};
        RefPtr<WebContentPage> source_page {};
    };
    static NonnullRefPtr<CanonicalNavigation> create(Start start)
    {
        return adopt_ref(*new CanonicalNavigation(move(start)));
    }

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
    // Held so that revoking the blob URL the navigation is to cannot take the entry away from it.
    BlobURLHandle blob_url;
    // The page hosting the source document, as checked when another page requested the navigation.
    RefPtr<WebContentPage> source_page {};

    RefPtr<WebContentPage> worker() const;
    RefPtr<WebContentPage> host() const;
    RefPtr<NavigationLoader> loader() const;
    bool is_finalizing() const { return state.has<Finalizing>(); }
    // AD-HOC: A later navigation doesn't end a javascript: URL's evaluation, only stopping does (whatwg/html#12120).
    bool ends_when_superseded() const { return !is_finalizing() && !state.has<EvaluatingJavaScriptURL>(); }

private:
    explicit CanonicalNavigation(Start start)
        : url(move(start.url))
        , navigation_id(move(start.navigation_id))
        , retry(move(start.retry))
        , sequence_number(start.sequence_number)
        , state(move(start.state))
        , page(move(start.page))
        , history_entry(move(start.history_entry))
        , source_page(move(start.source_page))
    {
    }
};

WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::PreparedNavigationDescriptor);
WEBVIEW_API Web::HTML::PreparedNavigationDescriptor prepare_navigation_to_retry(Web::HTML::NavigationStartRequest const&);

}
