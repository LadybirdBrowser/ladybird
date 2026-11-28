/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/RootVector.h>
#include <LibWeb/DOM/RustFFI.h>

namespace Web::DOM {

// A list of nodes that libweb_rust fills and reads, and that roots them for as long as it lives, see
// Rust/src/dom/rooted_nodes.rs. It lives in the frame of the C++ entry point that calls into Rust, below the Rust code
// that uses it.
class RootedNodeList final : public GC::RootVectorBase {
public:
    RootedNodeList() = default;
    ~RootedNodeList();

    RustFFI::RootedNodeListCore const& core() const { return m_core; }

    virtual void gather_roots(HashMap<GC::Cell*, GC::HeapRoot>&) const override;

private:
    RustFFI::RootedNodeListCore m_core { nullptr, 0, 0 };
};

}
