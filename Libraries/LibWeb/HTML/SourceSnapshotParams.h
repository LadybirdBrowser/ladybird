/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#source-snapshot-params
struct SourceSnapshotParams : public GC::Cell {
    GC_CELL(SourceSnapshotParams, GC::Cell)
    GC_DECLARE_ALLOCATOR(SourceSnapshotParams);

public:
    SourceSnapshotParams(bool has_transient_activation, SandboxingFlagSet sandboxing_flags, bool allows_downloading, GC::Ptr<EnvironmentSettingsObject> fetch_client, NonnullRefPtr<PolicyContainer> source_policy_container, bool text_directive_user_activation = false, bool browsing_context_group_has_multiple_contexts = false, bool browsing_context_group_has_remote_contexts = false)
        : has_transient_activation(has_transient_activation)
        , sandboxing_flags(sandboxing_flags)
        , allows_downloading(allows_downloading)
        , fetch_client(fetch_client)
        , source_policy_container(source_policy_container)
        , text_directive_user_activation(text_directive_user_activation)
        , browsing_context_group_has_multiple_contexts(browsing_context_group_has_multiple_contexts)
        , browsing_context_group_has_remote_contexts(browsing_context_group_has_remote_contexts)
    {
    }

    virtual ~SourceSnapshotParams() = default;

    // a boolean
    bool has_transient_activation;

    // a sandboxing flag set
    SandboxingFlagSet sandboxing_flags = {};

    // a boolean
    bool allows_downloading;

    // an environment settings object or null, only to be used as a request client
    GC::Ptr<EnvironmentSettingsObject> fetch_client;

    // a policy container
    NonnullRefPtr<PolicyContainer> source_policy_container;
    bool text_directive_user_activation { false };
    bool browsing_context_group_has_multiple_contexts { false };
    bool browsing_context_group_has_remote_contexts { false };

protected:
    virtual void visit_edges(Cell::Visitor&) override;
};

GC::Ref<SourceSnapshotParams> snapshot_source_snapshot_params(GC::Ptr<DOM::Document> source_document);

}
