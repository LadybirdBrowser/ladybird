/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Rootable.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/RootedNodeList.h>

namespace Web::DOM {

// NB: The Rust definition of the core asserts the same layout.
static_assert(sizeof(RustFFI::RootedNodeListCore) == 24 && alignof(RustFFI::RootedNodeListCore) == 8);

RootedNodeList::~RootedNodeList()
{
    RustFFI::rust_dom_rooted_node_list_destroy(&m_core);
}

void RootedNodeList::gather_roots(HashMap<GC::Cell*, GC::HeapRoot>& roots) const
{
    for (size_t index = 0; index < m_core.length; ++index)
        GC::Detail::gather_root(roots, m_core.nodes[index], GC::HeapRoot::Type::RootVector);
}

}
