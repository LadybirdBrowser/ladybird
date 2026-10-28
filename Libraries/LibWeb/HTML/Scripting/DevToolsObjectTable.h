/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/WebView/Debugger.h>

namespace Web::HTML {

// The objects that DevTools refers to by ID, along with the previews it shows of them. The objects stay for as long as
// DevTools may ask about them: The table either roots them, or leaves them for its owner to visit.
class WEB_API DevToolsObjectTable {
public:
    enum class RootObjects : u8 {
        No,
        Yes,
    };

    explicit DevToolsObjectTable(RootObjects root_objects)
        : m_root_objects(root_objects)
    {
    }

    WebView::DebuggerValue serialize_value(JS::Value);
    JsonValue serialize_value_as_grip(JS::Value);
    ErrorOr<WebView::DebuggerObjectProperties> properties_for_object(u64 object_id);
    void clear();
    void visit_edges(GC::Cell::Visitor&);

private:
    WebView::DebuggerValue serialize_value(JS::Value, u8 depth);
    u64 id_for_object(JS::Object&);
    JsonValue grip_value(JS::Value, u8 depth);
    JsonObject grip_fields_for_object(JS::Object&, u8 depth);

    RootObjects m_root_objects { RootObjects::No };
    HashMap<u64, GC::Ref<JS::Object>> m_objects;
    HashMap<GC::Ptr<JS::Object>, u64> m_object_ids;
    Vector<GC::Root<JS::Object>> m_roots;
};

}
