/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WorkerEnvironmentSettingsObject final
    : public EnvironmentSettingsObject {
    GC_CELL(WorkerEnvironmentSettingsObject, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(WorkerEnvironmentSettingsObject);

public:
    WorkerEnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> execution_context, GC::Ref<WorkerGlobalScope> global_scope, HighResolutionTime::DOMHighResTimeStamp unsafe_worker_creation_time)
        : EnvironmentSettingsObject(move(execution_context))
        , m_global_scope(global_scope)
        , m_unsafe_worker_creation_time(unsafe_worker_creation_time)
    {
    }

    static GC::Ref<WorkerEnvironmentSettingsObject> setup(GC::Ref<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, SerializedEnvironmentSettingsObject const& outside_settings, HighResolutionTime::DOMHighResTimeStamp unsafe_worker_creation_time);

    virtual ~WorkerEnvironmentSettingsObject() override = default;

    GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    String api_url_character_encoding() const override { return m_api_url_character_encoding; }
    URL::URL api_base_url() const override;
    URL::Origin origin() const override;
    PolicyContainer policy_container() const override;
    CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override;
    double time_origin() const override;

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    String m_api_url_character_encoding;
    URL::Origin m_origin;

    GC::Ref<WorkerGlobalScope> m_global_scope;

    HighResolutionTime::DOMHighResTimeStamp m_unsafe_worker_creation_time { 0 };
};

}
