/*
 * Copyright (c) 2025, Bogi Napoleon Wennerstrøm <bogi.wennerstrom@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Lock.h"

#include <AK/String.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebLocks {

GC_DEFINE_ALLOCATOR(Lock);

WebIDL::ExceptionOr<GC::Ref<Lock>> Lock::construct_impl(JS::Realm& realm, String const& name, Bindings::LockMode mode)
{
    return realm.create<Lock>(realm, name, mode);
}

Lock::Lock(JS::Realm& realm, String const& name, Bindings::LockMode mode)
    : PlatformObject(realm)
{
    this->m_name = name;
    this->m_mode = mode;
}

void Lock::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Lock);
}

String Lock::name() const
{
    return this->m_name;
}
Bindings::LockMode Lock::mode() const
{
    return this->m_mode;
}

}
