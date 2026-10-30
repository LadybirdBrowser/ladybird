/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Scripting/DevToolsObjectTable.h>

namespace Web::HTML {

// The objects of the messages that a page logs to its console, which DevTools may expand. Only the latest messages keep
// theirs, so a page that logs in a loop doesn't keep alive everything it ever logged: A message's objects go into the
// current table, and once that holds MESSAGES_PER_TABLE messages, it replaces the previous table.
class WEB_API DevToolsLoggedObjects {
public:
    static constexpr size_t MESSAGES_PER_TABLE = 500;

    Vector<JsonValue> serialize_message_arguments(ReadonlySpan<JS::Value>);
    bool contains(u64 object_id) const;
    ErrorOr<WebView::DebuggerObjectProperties> properties_for_object(u64 object_id);
    void clear();
    void visit_edges(GC::Cell::Visitor&);

private:
    DevToolsObjectTable m_current_table { DevToolsObjectTable::RootObjects::No };
    DevToolsObjectTable m_previous_table { DevToolsObjectTable::RootObjects::No };
    size_t m_messages_in_current_table { 0 };
};

}
