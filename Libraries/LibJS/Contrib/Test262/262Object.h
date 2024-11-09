/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Contrib/Test262/AgentObject.h>
#include <LibJS/Contrib/Test262/IsHTMLDDA.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>

namespace JS::Test262 {

class $262Object final : public Object {
    JS_OBJECT($262Object, Object);
    JS_DECLARE_ALLOCATOR($262Object);

public:
    virtual void initialize(Realm&) override;
    virtual ~$262Object() override = default;

private:
    explicit $262Object(Realm&);

    virtual void visit_edges(Visitor&) override;

    GCPtr<AgentObject> m_agent;
    GCPtr<IsHTMLDDA> m_is_htmldda;

    JS_DECLARE_NATIVE_FUNCTION(clear_kept_objects);
    JS_DECLARE_NATIVE_FUNCTION(create_realm);
    JS_DECLARE_NATIVE_FUNCTION(detach_array_buffer);
    JS_DECLARE_NATIVE_FUNCTION(eval_script);
};

}
