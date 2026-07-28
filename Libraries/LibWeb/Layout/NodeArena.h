/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>

namespace Web::Layout {

class TextNode;

static_assert(sizeof(RustFFI::NodeAllocation) == 24);
static_assert(offsetof(RustFFI::NodeAllocation, slot) == 0);
static_assert(offsetof(RustFFI::NodeAllocation, data) == 8);
static_assert(offsetof(RustFFI::NodeAllocation, generation) == 16);

class WEB_API NodeArena : public RefCounted<NodeArena> {
    AK_MAKE_NONCOPYABLE(NodeArena);
    AK_MAKE_NONMOVABLE(NodeArena);

public:
    NodeArena();
    ~NodeArena();

    RustFFI::NodeAllocation allocate();
    void free(RustFFI::NodeSlotId, u32 generation);
    void* handle() const { return m_handle; }

    void enroll_text_node_for_content_sync(TextNode const&);
    void sync_enrolled_text_node_content();

private:
    void* m_handle { nullptr };
    Vector<WeakPtr<TextNode>> m_text_nodes_enrolled_for_content_sync;
};

}
