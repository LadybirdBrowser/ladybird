/*
 * Copyright (c) 2020-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

class BigIntObject final : public Object {
    JS_OBJECT(BigIntObject, Object);
    GC_DECLARE_ALLOCATOR(BigIntObject);

public:
    static GC::Ref<BigIntObject> create(Realm&, BigInt&);

    virtual ~BigIntObject() override = default;

    BigInt const& bigint() const { return m_bigint; }
    BigInt& bigint() { return m_bigint; }

private:
    BigIntObject(BigInt&, Object& prototype);

    virtual bool is_bigint_object() const final { return true; }

    virtual void visit_edges(Visitor&) override;

    GC::Ref<BigInt> m_bigint;
};

template<>
inline bool Object::fast_is<BigIntObject>() const { return is_bigint_object(); }

}
