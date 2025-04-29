/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayIterator.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ArrayIterator);

// 23.1.5.1 CreateArrayIterator ( array, kind ), https://tc39.es/ecma262/#sec-createarrayiterator
GC::Ref<ArrayIterator> ArrayIterator::create(Realm& realm, Value array, Object::PropertyKind iteration_kind)
{
    // 1. Let iterator be OrdinaryObjectCreate(%ArrayIteratorPrototype%, « [[IteratedArrayLike]], [[ArrayLikeNextIndex]], [[ArrayLikeIterationKind]] »).
    // 2. Set iterator.[[IteratedArrayLike]] to array.
    // 3. Set iterator.[[ArrayLikeNextIndex]] to 0.
    // 4. Set iterator.[[ArrayLikeIterationKind]] to kind.
    // 5. Return iterator.
    return realm.create<ArrayIterator>(array, iteration_kind, realm.intrinsics().array_iterator_prototype());
}

ArrayIterator::ArrayIterator(Value array, Object::PropertyKind iteration_kind, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_array(array)
    , m_iteration_kind(iteration_kind)
{
}

void ArrayIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_array);
}

}
