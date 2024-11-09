/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayIterator.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ArrayIterator);

NonnullGCPtr<ArrayIterator> ArrayIterator::create(Realm& realm, Value array, Object::PropertyKind iteration_kind)
{
    return realm.heap().allocate<ArrayIterator>(realm, array, iteration_kind, realm.intrinsics().array_iterator_prototype());
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
