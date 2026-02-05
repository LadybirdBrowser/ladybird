/*
 * Copyright (c) 2025, Artsiom Yafremau <aplefull@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/RawJSONObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(RawJSONObject);

GC::Ref<RawJSONObject> RawJSONObject::create(Realm& realm, Object* prototype)
{
    if (!prototype)
        return realm.create<RawJSONObject>(realm.intrinsics().empty_object_shape());

    return realm.create<RawJSONObject>(ConstructWithPrototypeTag::Tag, *prototype);
}

}
