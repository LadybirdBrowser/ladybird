/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/Database.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(Database);

Database::~Database() = default;

JS::NonnullGCPtr<Database> Database::create(JS::Realm& realm, String const& name)
{
    return realm.heap().allocate<Database>(realm, realm, name);
}

void Database::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_associated_connections);
}


}
