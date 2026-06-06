/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSTransformComponent.h"
#include <LibWeb/Bindings/CSSTransformComponent.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSTransformComponent);

CSSTransformComponent::CSSTransformComponent(JS::Realm& realm, Is2D is_2d)
    : Bindings::Wrappable(realm)
    , m_is_2d(is_2d == Is2D::Yes)
{
}

CSSTransformComponent::~CSSTransformComponent() = default;

}
