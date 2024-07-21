/*
 * Copyright (c) 2020, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, circl <circl.lastname@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Utils.h>

namespace Web::DOM {

// https://dom.spec.whatwg.org/#retarget
EventTarget* retarget(EventTarget* a, EventTarget* b)
{
    // To retarget an object A against an object B, repeat these steps until they return an object:
    for (;;) {
        // 1. If one of the following is true then return A.
        // - A is not a node
        if (!is<Node>(a))
            return a;

        // - A’s root is not a shadow root
        auto* a_node = verify_cast<Node>(a);
        auto& a_root = a_node->root();
        if (!is<ShadowRoot>(a_root))
            return a;

        // - B is a node and A’s root is a shadow-including inclusive ancestor of B
        if (is<Node>(b) && a_root.is_shadow_including_inclusive_ancestor_of(verify_cast<Node>(*b)))
            return a;

        // 2. Set A to A’s root’s host.
        auto& a_shadow_root = verify_cast<ShadowRoot>(a_root);
        a = a_shadow_root.host();
    }
}

}
