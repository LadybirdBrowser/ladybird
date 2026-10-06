/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class JS_API BigIntObject final : public Object {
public:
    static GC::Ref<BigIntObject> create(Realm&, BigInt&);

    BigInt const& bigint() const;
    BigInt& bigint();

    static bool is_engine_class_of(Object const& object) { return object.engine_class_id() == JS_LAYOUT_CLASS_ID_BIG_INT_OBJECT; }
};

}
