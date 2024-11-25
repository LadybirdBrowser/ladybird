/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/Cell.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::HTML {

class WindowEnvironmentSettingsObject final : public EnvironmentSettingsObject {
    GC_CELL(WindowEnvironmentSettingsObject, EnvironmentSettingsObject);
    GC_DECLARE_ALLOCATOR(WindowEnvironmentSettingsObject);

public:
    static void setup(Page&, URL::URL const& creation_url, NonnullOwnPtr<JS::ExecutionContext>, GC::Ptr<Environment>, URL::URL top_level_creation_url, URL::Origin top_level_origin);

    virtual ~WindowEnvironmentSettingsObject() override;

    virtual GC::Ptr<DOM::Document> responsible_document() override;
    virtual String api_url_character_encoding() const override;
    virtual URL::URL api_base_url() const override;
    virtual URL::Origin origin() const override;
    virtual GC::Ref<PolicyContainer> policy_container() const override;
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() const override;
    virtual double time_origin() const override;

private:
    WindowEnvironmentSettingsObject(Window&, NonnullOwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ptr<Window> m_window;
};

}
