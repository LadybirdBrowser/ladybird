/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/NumberObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(NumberObject);

GC::Ref<NumberObject> NumberObject::create(Realm& realm, double value)
{
    return realm.create<NumberObject>(value, realm.intrinsics().number_prototype());
}

NumberObject::NumberObject(double value, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_value(value)
{
}

}
