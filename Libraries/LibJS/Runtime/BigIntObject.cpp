/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(BigIntObject);

GC::Ref<BigIntObject> BigIntObject::create(Realm& realm, BigInt& bigint)
{
    return realm.create<BigIntObject>(bigint, realm.intrinsics().bigint_prototype());
}

BigIntObject::BigIntObject(BigInt& bigint, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_bigint(bigint)
{
}

void BigIntObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_bigint);
}

}
