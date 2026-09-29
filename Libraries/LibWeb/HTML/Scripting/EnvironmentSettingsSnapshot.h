/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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
    virtual GC::Ref<PolicyContainer> policy_container() const override { return m_policy_container; }
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override { return CanUseCrossOriginIsolatedAPIs::No; }
    virtual Optional<u64> agent_cluster_id() const override { return m_agent_cluster_id; }
    virtual double time_origin() const override { return m_time_origin; }

    // The global object of the environment this was taken from, which its own global object isn't: A snapshot runs in
    // a realm of the process it's used in.
    SerializedGlobal const& serialized_global() const { return m_serialized_global; }

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    URL::URL m_url;
    URL::Origin m_origin;
    bool m_has_cross_site_ancestor;
    GC::Ref<PolicyContainer> m_policy_container;
    Optional<u64> m_agent_cluster_id;
    double m_time_origin { 0 };
    SerializedGlobal m_serialized_global;
};

}
