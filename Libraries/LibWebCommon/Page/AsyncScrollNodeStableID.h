/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web {

enum class AsyncScrollNodeKind : u8 {
    Viewport,
    Element,
    PseudoElement,
};

// Stable identity for reconciling compositor-side scroll offsets after the paint snapshot has been rebuilt.
struct AsyncScrollNodeStableID {
    UniqueNodeID node_id;
    AsyncScrollNodeKind kind { AsyncScrollNodeKind::Element };
    u8 pseudo_element_type { 0 };

    bool operator==(AsyncScrollNodeStableID const&) const = default;
};

// Where the compositor has scrolled a scroll node to, in CSS pixels.
struct CompositorScrollOffset {
    AsyncScrollNodeStableID scroll_node;
    CSSPixelPoint offset;

    bool operator==(CompositorScrollOffset const&) const = default;
};

// The compositor drags the thumb of a scrollbar the display list paints, while the mouse events of that drag still
// reach the main thread so that everything but the scrolling stays there.
struct ScrollbarDraggedByCompositor {
    AsyncScrollNodeStableID scroller_stable_node_id;
    bool vertical { false };

    bool operator==(ScrollbarDraggedByCompositor const&) const = default;
};

}

template<>
struct AK::Traits<Web::AsyncScrollNodeStableID> : DefaultTraits<Web::AsyncScrollNodeStableID> {
    static unsigned hash(Web::AsyncScrollNodeStableID const& stable_node_id)
    {
        return pair_int_hash(u64_hash(static_cast<u64>(stable_node_id.node_id.value())),
            pair_int_hash(to_underlying(stable_node_id.kind), stable_node_id.pseudo_element_type));
    }
};

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::AsyncScrollNodeStableID const&);
template<>
WEBCOMMON_API ErrorOr<Web::AsyncScrollNodeStableID> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::CompositorScrollOffset const&);
template<>
WEBCOMMON_API ErrorOr<Web::CompositorScrollOffset> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::ScrollbarDraggedByCompositor const&);
template<>
WEBCOMMON_API ErrorOr<Web::ScrollbarDraggedByCompositor> decode(Decoder&);

}
