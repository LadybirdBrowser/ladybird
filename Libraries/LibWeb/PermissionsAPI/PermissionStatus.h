/*
 * Copyright (c) 2026, Niccolo Antonelli-Dziri <niccolo.antonelli-dziri@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PermissionsAPI/Permissions.h>

namespace Web::PermissionsAPI {

class WEB_API PermissionStatus : public DOM::EventTarget {
    WEB_WRAPPABLE(PermissionStatus, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(PermissionStatus);

public:
    static GC::Ref<PermissionStatus> create(GC::Ref<DOM::EventTarget> relevant_global_object, PermissionDescriptor);

    // https://w3c.github.io/permissions/#dom-permissionstatus-state
    PermissionState state() const;
    void set_state(PermissionState state) { m_state = state; }

    // https://w3c.github.io/permissions/#dom-permissionstatus-name
    String const& name() const { return m_name; }

    PermissionDescriptor const& query() const { return m_query; }

    // https://w3c.github.io/permissions/#dfn-permissionstatus-update-steps
    void update_steps();

    // https://w3c.github.io/permissions/#dom-permissionstatus-onchange
    void set_onchange(GC::Ptr<WebIDL::CallbackType> event_handler);
    GC::Ptr<WebIDL::CallbackType> onchange();

private:
    PermissionStatus(GC::Ref<DOM::EventTarget> relevant_global_object, String const&,
        PermissionDescriptor);
    JS::Object& relevant_global_object() const;
    virtual void visit_edges(Cell::Visitor&) override;

    String m_name;
    PermissionState m_state { PermissionState::Prompt };

    // https://w3c.github.io/permissions/#dfn-query
    PermissionDescriptor m_query;

    GC::Ref<DOM::EventTarget> m_global_object;
};

}
