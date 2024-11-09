/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AggregateError.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(AggregateError);

NonnullGCPtr<AggregateError> AggregateError::create(Realm& realm)
{
    return realm.heap().allocate<AggregateError>(realm, realm.intrinsics().aggregate_error_prototype());
}

AggregateError::AggregateError(Object& prototype)
    : Error(prototype)
{
}

}
