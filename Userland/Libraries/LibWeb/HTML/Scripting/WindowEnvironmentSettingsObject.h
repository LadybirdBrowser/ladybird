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
    JS_CELL(WindowEnvironmentSettingsObject, EnvironmentSettingsObject);
    JS_DECLARE_ALLOCATOR(WindowEnvironmentSettingsObject);

public:
    static void setup(Page&, URL::URL const& creation_url, NonnullOwnPtr<JS::ExecutionContext>, JS::GCPtr<Environment>, URL::URL top_level_creation_url, URL::Origin top_level_origin);

    virtual ~WindowEnvironmentSettingsObject() override;

    virtual JS::GCPtr<DOM::Document> responsible_document() override;
    virtual String api_url_character_encoding() override;
    virtual URL::URL api_base_url() override;
    virtual URL::Origin origin() override;
    virtual PolicyContainer policy_container() override;
    virtual CanUseCrossOriginIsolatedAPIs cross_origin_isolated_capability() override;

private:
    WindowEnvironmentSettingsObject(Window&, NonnullOwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::GCPtr<Window> m_window;
};

}
