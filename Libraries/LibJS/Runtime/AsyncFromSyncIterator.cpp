/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AsyncFromSyncIterator.h>
#include <LibJS/Runtime/AsyncFromSyncIteratorPrototype.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AsyncFromSyncIterator);

GC::Ref<AsyncFromSyncIterator> AsyncFromSyncIterator::create(Realm& realm, GC::Ref<IteratorRecord> sync_iterator_record)
{
    return realm.create<AsyncFromSyncIterator>(realm, sync_iterator_record);
}

AsyncFromSyncIterator::AsyncFromSyncIterator(Realm& realm, GC::Ref<IteratorRecord> sync_iterator_record)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().async_from_sync_iterator_prototype())
    , m_sync_iterator_record(sync_iterator_record)
{
}

void AsyncFromSyncIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_sync_iterator_record);
}

}
