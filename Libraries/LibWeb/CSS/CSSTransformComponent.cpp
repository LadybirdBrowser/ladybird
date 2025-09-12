/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSTransformComponent.h"
#include <LibWeb/Bindings/CSSTransformComponentPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSTransformComponent);

CSSTransformComponent::CSSTransformComponent(JS::Realm& realm, Is2D is_2d)
    : Bindings::PlatformObject(realm)
    , m_is_2d(is_2d == Is2D::Yes)
{
}

CSSTransformComponent::~CSSTransformComponent() = default;

void CSSTransformComponent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSTransformComponent);
    Base::initialize(realm);
}

}
