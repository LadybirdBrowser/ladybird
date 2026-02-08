/*
 * Copyright (c) 2022, Ben Abraham <ben.d.abraham@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class WEB_API WorkerEnvironmentSettingsObject final
    : public EnvironmentSettingsObject {
    GC_CELL(WorkerEnvironmentSettingsObject, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(WorkerEnvironmentSettingsObject);

public:
    WorkerEnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> execution_context, GC::Ref<WorkerGlobalScope> global_scope, URL::Origin origin, bool outside_settings_has_cross_site_ancestor, HighResolutionTime::DOMHighResTimeStamp unsafe_worker_creation_time)
        : EnvironmentSettingsObject(move(execution_context))
        , m_origin(move(origin))
        , m_outside_settings_has_cross_site_ancestor(outside_settings_has_cross_site_ancestor)
        , m_global_scope(global_scope)
        , m_unsafe_worker_creation_time(unsafe_worker_creation_time)
    {
    }

    static GC::Ref<WorkerEnvironmentSettingsObject> setup(GC::Ref<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, SerializedEnvironmentSettingsObject const& outside_settings, HighResolutionTime::DOMHighResTimeStamp unsafe_worker_creation_time);

    virtual ~WorkerEnvironmentSettingsObject() override = default;

    virtual GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    virtual URL::URL api_base_url() const override;
    virtual URL::Origin origin() const override;
    virtual bool has_cross_site_ancestor() const override;
    virtual GC::Ref<PolicyContainer> policy_container() const override;
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override;
    virtual double time_origin() const override;

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;

    URL::Origin m_origin;
    bool m_outside_settings_has_cross_site_ancestor;

    GC::Ref<WorkerGlobalScope> m_global_scope;

    HighResolutionTime::DOMHighResTimeStamp m_unsafe_worker_creation_time { 0 };
};

}
