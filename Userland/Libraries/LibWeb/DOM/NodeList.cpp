/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/NodeListPrototype.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

NodeList::NodeList(JS::Realm& realm)
    : PlatformObject(realm)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .supports_indexed_properties = true };
}

NodeList::~NodeList() = default;

void NodeList::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(NodeList);
}

Optional<JS::Value> NodeList::item_value(size_t index) const
{
    auto* node = item(index);
    if (!node)
        return {};
    return const_cast<Node*>(node);
}

}
