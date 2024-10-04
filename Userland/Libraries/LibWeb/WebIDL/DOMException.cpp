/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMExceptionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebIDL {

JS_DEFINE_ALLOCATOR(DOMException);

JS::NonnullGCPtr<DOMException> DOMException::create(JS::Realm& realm, FlyString const& name, FlyString const& message)
{
    return realm.heap().allocate<DOMException>(realm, realm, name, message);
}

JS::NonnullGCPtr<DOMException> DOMException::construct_impl(JS::Realm& realm, FlyString const& message, FlyString const& name)
{
    return realm.heap().allocate<DOMException>(realm, realm, name, message);
}

DOMException::DOMException(JS::Realm& realm, FlyString const& name, FlyString const& message)
    : PlatformObject(realm)
    , m_name(name)
    , m_message(message)
{
}

DOMException::~DOMException() = default;

void DOMException::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMException);
}

}
