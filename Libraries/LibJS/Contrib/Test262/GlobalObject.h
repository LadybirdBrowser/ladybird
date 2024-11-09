/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Contrib/Test262/262Object.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS::Test262 {

class GlobalObject final : public JS::GlobalObject {
    JS_OBJECT(GlobalObject, JS::GlobalObject);
    JS_DECLARE_ALLOCATOR(GlobalObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~GlobalObject() override = default;

    $262Object* $262() const { return m_$262; }

private:
    GlobalObject(JS::Realm& realm)
        : JS::GlobalObject(realm)
    {
    }

    virtual void visit_edges(Visitor&) override;

    GCPtr<$262Object> m_$262;

    JS_DECLARE_NATIVE_FUNCTION(print);
};

}
