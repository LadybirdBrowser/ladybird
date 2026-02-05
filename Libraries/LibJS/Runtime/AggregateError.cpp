/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AggregateError.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

GC_DEFINE_ALLOCATOR(AggregateError);

GC::Ref<AggregateError> AggregateError::create(Realm& realm)
{
    return realm.create<AggregateError>(realm.intrinsics().aggregate_error_prototype());
}

AggregateError::AggregateError(Object& prototype)
    : Error(prototype)
{
}

}
