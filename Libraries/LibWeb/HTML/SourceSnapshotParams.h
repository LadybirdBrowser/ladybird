/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
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
    SourceSnapshotParams(bool has_transient_activation, SandboxingFlagSet sandboxing_flags, bool allows_downloading, GC::Ref<EnvironmentSettingsObject> fetch_client, GC::Ref<PolicyContainer> source_policy_container)
        : has_transient_activation(has_transient_activation)
        , sandboxing_flags(sandboxing_flags)
        , allows_downloading(allows_downloading)
        , fetch_client(fetch_client)
        , source_policy_container(source_policy_container)
    {
    }

    virtual ~SourceSnapshotParams() = default;

    // a boolean
    bool has_transient_activation;

    // a sandboxing flag set
    SandboxingFlagSet sandboxing_flags = {};

    // a boolean
    bool allows_downloading;

    // an environment settings object, only to be used as a request client
    GC::Ref<EnvironmentSettingsObject> fetch_client;

    // a policy container
    GC::Ref<PolicyContainer> source_policy_container;

protected:
    virtual void visit_edges(Cell::Visitor&) override;
};

}
