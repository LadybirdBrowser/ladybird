/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class ConsoleObject final : public Object {
    JS_OBJECT(ConsoleObject, Object);
    JS_DECLARE_ALLOCATOR(ConsoleObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~ConsoleObject() override;

    Console& console() { return *m_console; }

private:
    explicit ConsoleObject(Realm&);
    virtual void visit_edges(Visitor&) override;

    JS_DECLARE_NATIVE_FUNCTION(assert_);
    JS_DECLARE_NATIVE_FUNCTION(clear);
    JS_DECLARE_NATIVE_FUNCTION(debug);
    JS_DECLARE_NATIVE_FUNCTION(error);
    JS_DECLARE_NATIVE_FUNCTION(info);
    JS_DECLARE_NATIVE_FUNCTION(log);
    JS_DECLARE_NATIVE_FUNCTION(trace);
    JS_DECLARE_NATIVE_FUNCTION(warn);
    JS_DECLARE_NATIVE_FUNCTION(table);
    JS_DECLARE_NATIVE_FUNCTION(dir);
    JS_DECLARE_NATIVE_FUNCTION(count);
    JS_DECLARE_NATIVE_FUNCTION(count_reset);
    JS_DECLARE_NATIVE_FUNCTION(group);
    JS_DECLARE_NATIVE_FUNCTION(group_collapsed);
    JS_DECLARE_NATIVE_FUNCTION(group_end);
    JS_DECLARE_NATIVE_FUNCTION(time);
    JS_DECLARE_NATIVE_FUNCTION(time_log);
    JS_DECLARE_NATIVE_FUNCTION(time_end);

    GCPtr<Console> m_console;
};

}
