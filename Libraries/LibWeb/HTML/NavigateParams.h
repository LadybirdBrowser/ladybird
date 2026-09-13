/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/InitialInsertion.h>
#include <LibWeb/HTML/POSTResource.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/StructuredSerializeTypes.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::HTML {

// The arguments of https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
struct NavigateParams {
    URL::URL url;
    GC::Ptr<DOM::Document> source_document = nullptr;
    DocumentResource document_resource = Empty {};
    GC::Ptr<Fetch::Infrastructure::Response> response = nullptr;
    bool exceptions_enabled = false;
    Bindings::NavigationHistoryBehavior history_handling = Bindings::NavigationHistoryBehavior::Auto;
    Optional<StorageSerializationRecord> navigation_api_state = {};
    Optional<Vector<XHR::FormDataEntry>> form_data_entry_list = {};
    ReferrerPolicy::ReferrerPolicy referrer_policy = ReferrerPolicy::ReferrerPolicy::EmptyString;
    UserNavigationInvolvement user_involvement = UserNavigationInvolvement::None;
    // NB: A load requested by the UI process carries the ID the UI generated when it recorded the
    //     navigation; otherwise step 7 of the navigate algorithm generates one.
    Optional<Utf16String> navigation_id = {};
    GC::Ptr<DOM::Element> source_element = nullptr;
    InitialInsertion initial_insertion = InitialInsertion::No;
    GC::Ptr<NavigationAPIMethodTracker> api_method_tracker = nullptr;

    void visit_edges(GC::Cell::Visitor&);
};

struct PreparedNavigation {
    URL::URL url;
    DocumentResource document_resource;
    GC::Ptr<Fetch::Infrastructure::Response> response;
    Bindings::NavigationHistoryBehavior history_handling;
    Optional<StorageSerializationRecord> navigation_api_state;
    Optional<Vector<XHR::FormDataEntry>> form_data_entry_list;
    ReferrerPolicy::ReferrerPolicy referrer_policy;
    UserNavigationInvolvement user_involvement;
    Utf16String navigation_id;
    GC::Ptr<DOM::Element> source_element;
    InitialInsertion initial_insertion;
    GC::Ptr<NavigationAPIMethodTracker> api_method_tracker;
    ContentSecurityPolicy::Directives::Directive::NavigationType csp_navigation_type;
    GC::Ref<SourceSnapshotParams> source_snapshot_params;
    URL::Origin initiator_origin_snapshot;
    URL::URL initiator_base_url_snapshot;

    void visit_edges(GC::Cell::Visitor&);
};

}
