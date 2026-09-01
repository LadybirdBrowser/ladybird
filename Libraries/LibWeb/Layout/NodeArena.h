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
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::Layout {

class Node;
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
    u64 formatting_context_run_cache_hit_count() const;
    u64 table_cell_measurement_cache_miss_count() const;
    u64 intrinsic_measurement_count() const;

    void sync_enrolled_content_for_layout();

private:
    void* m_handle { nullptr };
};

WEB_API bool detach_layout_node_for_destruction(Node&);

}
