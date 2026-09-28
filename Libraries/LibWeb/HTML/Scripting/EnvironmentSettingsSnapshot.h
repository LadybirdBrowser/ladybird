/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWebCommon/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace Web::HTML {

class WEB_API EnvironmentSettingsSnapshot final
    : public EnvironmentSettingsObject {
    GC_CELL(EnvironmentSettingsSnapshot, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(EnvironmentSettingsSnapshot);

public:
    EnvironmentSettingsSnapshot(NonnullOwnPtr<JS::ExecutionContext>, SerializedEnvironmentSettingsObject const&);

    virtual ~EnvironmentSettingsSnapshot() override;

    virtual GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    virtual URL::URL api_base_url() const override { return m_url; }
    virtual URL::Origin origin() const override { return m_origin; }
    virtual bool has_cross_site_ancestor() const override { return m_has_cross_site_ancestor; }
    virtual NonnullRefPtr<PolicyContainer> policy_container() const override { return m_policy_container; }
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override { return m_cross_origin_isolated_capability; }
    virtual Optional<u64> agent_cluster_id() const override { return m_agent_cluster_id; }
    virtual CrossOriginIsolationMode agent_cluster_cross_origin_isolation_mode() const override { return m_agent_cluster_cross_origin_isolation_mode; }
    virtual double time_origin() const override { return m_time_origin; }

    // The global object of the environment this was taken from, which its own global object isn't: A snapshot runs in
    // a realm of the process it's used in.
    SerializedGlobal const& serialized_global() const { return m_serialized_global; }

    // The violations of this environment's policies need to be reported to its global object, in the process it's
    // from. If this is set, it does that, given the index of the violated policy in this environment's CSP list, the
    // violation's effective directive, and its resource.
    using ReportViolation = GC::Function<void(u64 policy_index, String effective_directive, URL::URL resource)>;
    GC::Ptr<ReportViolation> report_violation() const { return m_report_violation; }
    void set_report_violation(GC::Ref<ReportViolation> report_violation) { m_report_violation = report_violation; }

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    URL::URL m_url;
    URL::Origin m_origin;
    bool m_has_cross_site_ancestor;
    NonnullRefPtr<PolicyContainer> m_policy_container;
    CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { CanUseCrossOriginIsolatedAPIs::No };
    Optional<u64> m_agent_cluster_id;
    CrossOriginIsolationMode m_agent_cluster_cross_origin_isolation_mode { CrossOriginIsolationMode::None };
    double m_time_origin { 0 };
    SerializedGlobal m_serialized_global;
    GC::Ptr<ReportViolation> m_report_violation;
};

}
