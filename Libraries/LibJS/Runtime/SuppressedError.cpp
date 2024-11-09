/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/SuppressedError.h>

namespace JS {

JS_DEFINE_ALLOCATOR(SuppressedError);

NonnullGCPtr<SuppressedError> SuppressedError::create(Realm& realm)
{
    return realm.heap().allocate<SuppressedError>(realm, realm.intrinsics().suppressed_error_prototype());
}

SuppressedError::SuppressedError(Object& prototype)
    : Error(prototype)
{
}

}
