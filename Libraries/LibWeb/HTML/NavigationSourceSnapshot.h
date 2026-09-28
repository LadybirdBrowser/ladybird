/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/NavigationSourceSnapshot.h>

namespace Web::HTML {

WEB_API NavigationSourceSnapshot create_navigation_source_snapshot(SourceSnapshotParams const&);
WEB_API GC::Ref<SourceSnapshotParams> create_source_snapshot_params_from_navigation_source_snapshot(JS::Realm&, NavigationSourceSnapshot const&);

}
