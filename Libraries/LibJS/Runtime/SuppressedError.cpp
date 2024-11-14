/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/SuppressedError.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SuppressedError);

GC::Ref<SuppressedError> SuppressedError::create(Realm& realm)
{
    return realm.create<SuppressedError>(realm.intrinsics().suppressed_error_prototype());
}

SuppressedError::SuppressedError(Object& prototype)
    : Error(prototype)
{
}

}
