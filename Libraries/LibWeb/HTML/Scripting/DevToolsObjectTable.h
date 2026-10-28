/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/WebView/Debugger.h>

namespace Web::HTML {

// The objects that DevTools refers to by ID, kept alive for as long as DevTools may ask about them.
class WEB_API DevToolsObjectTable {
public:
    WebView::DebuggerValue serialize_value(JS::Value);
    ErrorOr<WebView::DebuggerObjectProperties> properties_for_object(u64 object_id);
    void clear();

private:
    HashMap<u64, GC::Root<JS::Object>> m_objects;
    HashMap<GC::Ptr<JS::Object>, u64> m_object_ids;
    u64 m_next_object_id { 1 };
};

}
