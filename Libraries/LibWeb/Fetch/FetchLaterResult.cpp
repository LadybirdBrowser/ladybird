/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/FetchLaterResultPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Fetch/FetchLaterResult.h>

namespace Web::Fetch {

GC_DEFINE_ALLOCATOR(FetchLaterResult);

// https://fetch.spec.whatwg.org/#fetchlaterresult
GC::Ref<FetchLaterResult> FetchLaterResult::create(JS::Realm& realm)
{
    return realm.create<FetchLaterResult>(realm);
}

FetchLaterResult::FetchLaterResult(JS::Realm& realm)
    : PlatformObject(realm)
{
}

FetchLaterResult::~FetchLaterResult() = default;

void FetchLaterResult::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FetchLaterResult);
    Base::initialize(realm);
}

}
