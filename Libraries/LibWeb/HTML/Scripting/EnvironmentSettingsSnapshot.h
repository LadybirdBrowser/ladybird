/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace Web::HTML {

class WEB_API EnvironmentSettingsSnapshot final
    : public EnvironmentSettingsObject {
    GC_CELL(EnvironmentSettingsSnapshot, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(EnvironmentSettingsSnapshot);

public:
    EnvironmentSettingsSnapshot(JS::Realm&, NonnullOwnPtr<JS::ExecutionContext>, SerializedEnvironmentSettingsObject const&);

    virtual ~EnvironmentSettingsSnapshot() override;

    virtual GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    virtual String api_url_character_encoding() const override { return m_api_url_character_encoding; }
    virtual URL::URL api_base_url() const override { return m_url; }
    virtual URL::Origin origin() const override { return m_origin; }
    virtual bool has_cross_site_ancestor() const override { return m_has_cross_site_ancestor; }
    virtual GC::Ref<PolicyContainer> policy_container() const override { return m_policy_container; }
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override { return CanUseCrossOriginIsolatedAPIs::No; }
    virtual double time_origin() const override { return m_time_origin; }

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    String m_api_url_character_encoding;
    URL::URL m_url;
    URL::Origin m_origin;
    bool m_has_cross_site_ancestor;
    GC::Ref<PolicyContainer> m_policy_container;
    double m_time_origin { 0 };
};

}
