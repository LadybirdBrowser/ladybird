/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Permissions.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::PermissionsAPI {

class WEB_API PermissionStatus : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(PermissionStatus, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(PermissionStatus);

public:
    static GC::Ref<PermissionStatus> create(JS::Realm&, Bindings::PermissionDescriptor);

    // https://w3c.github.io/permissions/#dom-permissionstatus-state
    Bindings::PermissionState state() const { return m_state; }
    void set_state(Bindings::PermissionState state) { m_state = state; }

    // https://w3c.github.io/permissions/#dom-permissionstatus-name
    String const& name() const { return m_name; }

    Bindings::PermissionDescriptor const& query() const { return m_query; }

    // https://w3c.github.io/permissions/#dfn-permissionstatus-update-steps
    void update_steps();

    // https://w3c.github.io/permissions/#dom-permissionstatus-onchange
    void set_onchange(GC::Ptr<WebIDL::CallbackType> event_handler);
    GC::Ptr<WebIDL::CallbackType> onchange();

private:
    PermissionStatus(JS::Realm&, String const&, Bindings::PermissionDescriptor const&);

    virtual void initialize(JS::Realm&) override;

    String m_name;
    Bindings::PermissionState m_state { Bindings::PermissionState::Prompt };

    // https://w3c.github.io/permissions/#dfn-query
    Bindings::PermissionDescriptor m_query;
};

}
