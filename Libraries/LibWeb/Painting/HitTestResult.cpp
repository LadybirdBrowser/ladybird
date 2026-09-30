/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/HitTestResult.h>

namespace Web::Painting {

DOM::Node* HitTestResult::dom_node() const
{
    auto* document = arena->document();
    if (!document)
        return nullptr;
    return node.resolve(*document).ptr();
}

}
