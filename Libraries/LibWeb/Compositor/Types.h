/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/DistinctNumeric.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Export.h>
#include <LibWeb/Painting/DisplayListResourceIds.h>

namespace Web::Compositor {

AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, CompositorContextId);
AK_TYPEDEF_DISTINCT_ORDERED_ID(u64, ScreenshotRequestId);

static constexpr u64 page_presenting_context_id_tag = 1ull << 63;

inline CompositorContextId allocate_compositor_context_id()
{
    static Atomic<u64> s_next_id { 1 };
    return CompositorContextId { s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) };
}

inline CompositorContextId compositor_context_id_for_page(u64 page_id)
{
    VERIFY((page_id & page_presenting_context_id_tag) == 0);
    return CompositorContextId { page_presenting_context_id_tag | page_id };
}

inline bool is_page_presenting_compositor_context_id(CompositorContextId context_id)
{
    return (context_id.value() & page_presenting_context_id_tag) != 0;
}

inline u64 page_id_for_compositor_context_id(CompositorContextId context_id)
{
    VERIFY(is_page_presenting_compositor_context_id(context_id));
    return context_id.value() & ~page_presenting_context_id_tag;
}

enum class WindowResizingInProgress : u8 {
    No,
    Yes,
};

enum class PagePresentationRegistration {
    No,
    Yes,
};

struct PendingAsyncScrollUpdates {
    Vector<AsyncScrollOffset> scroll_offsets;
    Vector<AsyncScrollOperationID> completed_operation_ids;
};

struct AsyncScrollEnqueueResult {
    bool accepted { false };
    Optional<AsyncScrollOperationID> operation_id;
};

enum class AsyncScrollOperationTracking {
    No,
    Yes,
};

struct PublishToCompositorSurface {
    CompositorContextId target_context_id;
    Painting::CompositorSurfaceId surface_id;
};

struct PresentToClient {
};

using PresentationMode = Variant<Empty, PresentToClient, PublishToCompositorSurface>;

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollNodeStableID const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollNodeStableID> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollOffset const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollOffset> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::PendingAsyncScrollUpdates const&);
template<>
WEB_API ErrorOr<Web::Compositor::PendingAsyncScrollUpdates> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::AsyncScrollEnqueueResult const&);
template<>
WEB_API ErrorOr<Web::Compositor::AsyncScrollEnqueueResult> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::PublishToCompositorSurface const&);
template<>
WEB_API ErrorOr<Web::Compositor::PublishToCompositorSurface> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Compositor::PresentToClient const&);
template<>
WEB_API ErrorOr<Web::Compositor::PresentToClient> decode(Decoder&);

}
