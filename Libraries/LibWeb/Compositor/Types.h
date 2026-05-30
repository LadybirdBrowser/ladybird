/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
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

inline CompositorContextId compositor_context_id_for_page(u64 page_id)
{
    VERIFY(page_id > 0);
    return CompositorContextId { page_id };
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
