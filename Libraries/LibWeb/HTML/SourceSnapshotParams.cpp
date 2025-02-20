/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SourceSnapshotParams);

void SourceSnapshotParams::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(fetch_client);
    visitor.visit(source_policy_container);
}

}
