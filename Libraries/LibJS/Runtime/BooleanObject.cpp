/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(BooleanObject);

NonnullGCPtr<BooleanObject> BooleanObject::create(Realm& realm, bool value)
{
    return realm.heap().allocate<BooleanObject>(realm, value, realm.intrinsics().boolean_prototype());
}

BooleanObject::BooleanObject(bool value, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_value(value)
{
}

}
