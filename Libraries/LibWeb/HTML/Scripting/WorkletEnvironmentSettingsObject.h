/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/worklets.html#set-up-a-worklet-environment-settings-object
class WEB_API WorkletEnvironmentSettingsObject final
    : public EnvironmentSettingsObject {
    GC_CELL(WorkletEnvironmentSettingsObject, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(WorkletEnvironmentSettingsObject);

public:
    static GC::Ref<WorkletEnvironmentSettingsObject> setup(GC::Ref<Page>, NonnullOwnPtr<JS::ExecutionContext>, GC::Ref<WorkletGlobalScope>, EnvironmentSettingsObject& outside_settings);

    virtual ~WorkletEnvironmentSettingsObject() override = default;

    virtual GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    virtual URL::URL api_base_url() const override { return m_api_base_url; }
    virtual URL::Origin origin() const override { return m_origin; }
    virtual bool has_cross_site_ancestor() const override { return m_has_cross_site_ancestor; }
    virtual GC::Ref<PolicyContainer> policy_container() const override;
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override { return m_cross_origin_isolated_capability; }
    virtual double time_origin() const override { return m_time_origin; }

private:
    WorkletEnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext>, GC::Ref<WorkletGlobalScope>, URL::URL api_base_url, URL::Origin, bool has_cross_site_ancestor, GC::Ref<PolicyContainer>, CanUseCrossOriginIsolatedAPIs, double time_origin);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<WorkletGlobalScope> m_global_scope;

    URL::URL m_api_base_url;
    URL::Origin m_origin;
    bool m_has_cross_site_ancestor { false };
    GC::Ref<PolicyContainer> m_policy_container;
    CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { CanUseCrossOriginIsolatedAPIs::No };
    double m_time_origin { 0 };
};

}
