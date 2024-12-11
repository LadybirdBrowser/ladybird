/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/SandboxingFlagSet.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#source-snapshot-params
struct SourceSnapshotParams : public JS::Cell {
    GC_CELL(SourceSnapshotParams, JS::Cell)
    GC_DECLARE_ALLOCATOR(SourceSnapshotParams);

public:
    virtual ~SourceSnapshotParams() = default;

    // a boolean
    bool has_transient_activation;

    // a sandboxing flag set
    SandboxingFlagSet sandboxing_flags = {};

    // a boolean
    bool allows_downloading;

    // an environment settings object, only to be used as a request client
    GC::Ptr<EnvironmentSettingsObject> fetch_client;

    // a policy container
    GC::Ptr<PolicyContainer> source_policy_container;

protected:
    virtual void visit_edges(Cell::Visitor&) override;
};

}
