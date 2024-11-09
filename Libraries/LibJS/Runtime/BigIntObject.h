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
    JS_DECLARE_ALLOCATOR(BigIntObject);

public:
    static NonnullGCPtr<BigIntObject> create(Realm&, BigInt&);

    virtual ~BigIntObject() override = default;

    BigInt const& bigint() const { return m_bigint; }
    BigInt& bigint() { return m_bigint; }

private:
    BigIntObject(BigInt&, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    NonnullGCPtr<BigInt> m_bigint;
};

}
