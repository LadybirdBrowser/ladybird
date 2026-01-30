/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::WebAudio {

class AudioWorkletGlobalScope;

class WEB_API AudioWorkletEnvironmentSettingsObject final : public HTML::EnvironmentSettingsObject {
    GC_CELL(AudioWorkletEnvironmentSettingsObject, HTML::EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(AudioWorkletEnvironmentSettingsObject);

public:
    static GC::Ref<AudioWorkletEnvironmentSettingsObject> setup(GC::Ref<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, HTML::SerializedEnvironmentSettingsObject const& outside_settings, URL::URL const& global_scope_url);

    virtual ~AudioWorkletEnvironmentSettingsObject() override = default;

    virtual GC::Ptr<DOM::Document> responsible_document() override { return nullptr; }
    virtual URL::URL api_base_url() const override;
    virtual URL::Origin origin() const override;
    virtual bool has_cross_site_ancestor() const override;
    virtual GC::Ref<HTML::PolicyContainer> policy_container() const override;
    virtual HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override;
    virtual double time_origin() const override;

private:
    AudioWorkletEnvironmentSettingsObject(NonnullOwnPtr<JS::ExecutionContext> execution_context, GC::Ref<AudioWorkletGlobalScope> global_scope, URL::URL global_scope_url, URL::Origin origin, bool outside_settings_has_cross_site_ancestor, GC::Ref<HTML::PolicyContainer> policy_container, HTML::CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability, double time_origin);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<AudioWorkletGlobalScope> m_global_scope;
    URL::URL m_global_scope_url;
    URL::Origin m_origin;
    bool m_outside_settings_has_cross_site_ancestor { false };
    GC::Ref<HTML::PolicyContainer> m_policy_container;
    HTML::CanUseCrossOriginIsolatedAPIs m_cross_origin_isolated_capability { HTML::CanUseCrossOriginIsolatedAPIs::No };
    double m_time_origin { 0 };
};

}
