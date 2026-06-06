/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>

namespace Web::DOM {

NodeList::NodeList()
    : Bindings::Wrappable()
{
}

NodeList::~NodeList() = default;

Optional<JS::Value> NodeList::item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, size_t index) const
{
    auto* node = item(index);
    if (!node)
        return {};
    return Bindings::wrap(wrapper_world, realm, GC::Ref { const_cast<Node&>(*node) }).ptr();
}

}
