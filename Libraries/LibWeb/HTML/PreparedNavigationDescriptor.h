/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWebCommon/HTML/PreparedNavigationDescriptor.h>

namespace Web::HTML {

WEB_API PreparedNavigationDescriptor create_prepared_navigation_descriptor(PreparedNavigation const&);
// What navigate takes from its source document in steps 2 to 4, 6.3, 6.4, and 7 -- and, with no source document, the
// opaque origin and about:blank those steps start from.
struct NavigationSourceSnapshots {
    GC::Ref<SourceSnapshotParams> source_snapshot_params;
    URL::Origin initiator_origin_snapshot;
    URL::URL initiator_base_url_snapshot;
    Utf16String navigation_id;
};
WEB_API NavigationSourceSnapshots snapshot_navigation_source(GC::Ptr<DOM::Document> source_document);

WEB_API PreparedNavigationDescriptor prepare_navigation_from_document(DOM::Document& source_document, URL::URL, ReferrerPolicy::ReferrerPolicy);

}
