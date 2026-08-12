/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>

namespace Web::Bindings {

WEB_API JS::Realm& main_world_realm(HTML::WorkletGlobalScope const&);

}

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/worklets.html#workletglobalscope
// NOTE: The IDL interface does not inherit from EventTarget; deriving from DOM::EventTarget here is
//       internal plumbing so worklet scopes fit the machinery that types "a global" as an event
//       target (MessagePort creation, relevant-global helpers). No EventTarget API is exposed.
class WEB_API WorkletGlobalScope
    : public DOM::EventTarget
    , public UniversalGlobalScopeMixin {
    WEB_WRAPPABLE(WorkletGlobalScope, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(WorkletGlobalScope);

public:
    virtual ~WorkletGlobalScope() override;

    virtual DOM::EventTarget& this_impl() override { return *this; }
    virtual DOM::EventTarget const& this_impl() const override { return *this; }

    JS::Realm& realm() const;

    EnvironmentSettingsObject& settings_object() const;
    void set_settings_object(Badge<WorkletEnvironmentSettingsObject>, GC::Ref<EnvironmentSettingsObject>);

    URL::URL const& url() const { return m_url; }
    void set_url(URL::URL url) { m_url = move(url); }

    // Called once the realm's [[HostDefined]] is installed; subclass overrides are generated from IDL
    // and install the global's exposed interfaces and mixin members.
    virtual void initialize_web_interfaces_impl() { }

protected:
    WorkletGlobalScope();

    virtual void visit_edges(Cell::Visitor&) override;

private:
    friend JS::Realm& Bindings::main_world_realm(WorkletGlobalScope const&);

    virtual bool is_universal_global_scope_mixin() const final { return true; }

    GC::Ptr<EnvironmentSettingsObject> m_settings_object;
    URL::URL m_url;
};

}
