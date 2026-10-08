/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/NavigateParams.h>
#include <LibWeb/HTML/NavigationSourceSnapshot.h>
#include <LibWeb/HTML/PreparedNavigationDescriptor.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>

namespace Web::HTML {

PreparedNavigationDescriptor create_prepared_navigation_descriptor(PreparedNavigation const& navigation)
{
    VERIFY(!navigation.response);
    VERIFY(!navigation.api_method_tracker);

    return {
        .url = navigation.url,
        .document_resource = navigation.document_resource,
        .history_handling = navigation.history_handling,
        .navigation_api_state = navigation.navigation_api_state,
        .referrer_policy = navigation.referrer_policy,
        .user_involvement = navigation.user_involvement,
        .navigation_id = navigation.navigation_id,
        .initial_insertion = navigation.initial_insertion,
        .csp_navigation_type = navigation.csp_navigation_type,
        .source_snapshot_params = create_navigation_source_snapshot(navigation.source_snapshot_params),
        .initiator_origin_snapshot = navigation.initiator_origin_snapshot,
        .initiator_base_url_snapshot = navigation.initiator_base_url_snapshot,
    };
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
NavigationSourceSnapshots snapshot_navigation_source(GC::Ptr<DOM::Document> source_document)
{
    // 2. Let sourceSnapshotParams be the result of snapshotting source snapshot params given sourceDocument.
    auto source_snapshot_params = snapshot_source_snapshot_params(source_document);

    // 3. Let initiatorOriginSnapshot be a new opaque origin.
    auto initiator_origin_snapshot = URL::Origin::create_opaque();

    // 4. Let initiatorBaseURLSnapshot be about:blank.
    auto initiator_base_url_snapshot = URL::about_blank();

    // 6. Otherwise:
    if (source_document) {
        //    3. Set initiatorOriginSnapshot to sourceDocument's origin.
        initiator_origin_snapshot = source_document->origin();

        //    4. Set initiatorBaseURLSnapshot to sourceDocument's document base URL.
        initiator_base_url_snapshot = source_document->base_url();
    }

    // 7. Let navigationId be the result of generating a random UUID.
    auto uuid = Crypto::generate_random_uuid();

    return {
        .source_snapshot_params = source_snapshot_params,
        .initiator_origin_snapshot = move(initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(initiator_base_url_snapshot),
        .navigation_id = Utf16String::from_ascii_without_validation(uuid.bytes()),
    };
}

// Steps 1 to 7 of navigate, for a navigation from sourceDocument that the UI process starts once it has the navigable:
// A link the user opens in a new tab, e.g., or an image opened from its context menu.
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
PreparedNavigationDescriptor prepare_navigation_from_document(DOM::Document& source_document, URL::URL url, ReferrerPolicy::ReferrerPolicy referrer_policy)
{
    // 1. Let cspNavigationType be "form-submission" if formDataEntryList is non-null; otherwise "other".
    auto csp_navigation_type = ContentSecurityPolicy::Directives::NavigationType::Other;

    // Steps 2 to 4, 6.3, 6.4, and 7.
    auto snapshots = snapshot_navigation_source(source_document);

    // 6. Otherwise:
    //    1. Assert: userInvolvement is not "browser UI".
    // NB: HTML leaves it to the user agent how the user gets to follow a link in a new tab. It's "activation" here,
    //     whether by a click or from the context menu: The URL is the page's, so the navigation has the page as its
    //     source — as section 4.3 of Fetch Metadata asks for a link opened from its context menu or with a Ctrl-click.
    //     https://w3c.github.io/webappsec-fetch-metadata/#directly-user-initiated
    auto user_involvement = UserNavigationInvolvement::Activation;

    //    2. If sourceDocument's node navigable is not allowed by sandboxing to navigate navigable given
    //       sourceSnapshotParams, then return.
    // NB: The user chose the navigable in the browser's UI, rather than the page choosing it, so its sandboxing has
    //     no say in it. And the UI process starts the navigation, so no navigate event is fired for it (step 21).

    return {
        .url = move(url),
        .document_resource = {},
        .history_handling = Bindings::NavigationHistoryBehavior::Auto,
        .navigation_api_state = {},
        .referrer_policy = referrer_policy,
        .user_involvement = user_involvement,
        .navigation_id = move(snapshots.navigation_id),
        .initial_insertion = InitialInsertion::No,
        .csp_navigation_type = csp_navigation_type,
        .source_snapshot_params = create_navigation_source_snapshot(snapshots.source_snapshot_params),
        .initiator_origin_snapshot = move(snapshots.initiator_origin_snapshot),
        .initiator_base_url_snapshot = move(snapshots.initiator_base_url_snapshot),
    };
}

}
