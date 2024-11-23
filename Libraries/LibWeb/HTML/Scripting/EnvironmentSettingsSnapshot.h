/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

namespace Web::HTML {

class EnvironmentSettingsSnapshot final
    : public EnvironmentSettingsObject {
    GC_CELL(EnvironmentSettingsSnapshot, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(EnvironmentSettingsSnapshot);

public:
    EnvironmentSettingsSnapshot(NonnullOwnPtr<JS::ExecutionContext>, SerializedEnvironmentSettingsObject const&);

    virtual ~EnvironmentSettingsSnapshot() override;

    GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    String api_url_character_encoding() const override { return m_api_url_character_encoding; }
    URL::URL api_base_url() const override { return m_url; }
    URL::Origin origin() const override { return m_origin; }
    PolicyContainer policy_container() const override { return m_policy_container; }
    CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override { return CanUseCrossOriginIsolatedAPIs::No; }

private:
    String m_api_url_character_encoding;
    URL::URL m_url;
    URL::Origin m_origin;
    HTML::PolicyContainer m_policy_container;
};

}
