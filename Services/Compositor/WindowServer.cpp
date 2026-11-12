/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/WindowServer.h>

#ifdef AK_OS_MACOS
#    include <Compositor/CoreAnimationWindowServer.h>
#endif

namespace Compositor {

RefPtr<WindowServer> WindowServer::try_create_for_presentation()
{
#ifdef AK_OS_MACOS
    return CoreAnimationWindowServer::try_create();
#else
    return nullptr;
#endif
}

ErrorOr<RefPtr<PresentableSurface>> WindowServer::try_create_presentable_surface_for_sublayers([[maybe_unused]] Gfx::IntSize size, [[maybe_unused]] Gfx::SkiaBackendContext& skia_backend_context)
{
#ifdef AK_OS_MACOS
    return RefPtr<PresentableSurface> { TRY(CoreAnimationWindowServer::try_create_presentable_surface_over_new_iosurface(size, skia_backend_context)) };
#else
    return nullptr;
#endif
}

WindowServer::FrameHandOff WindowServer::frame_hand_off_through_presentation()
{
#ifdef AK_OS_MACOS
    return FrameHandOff::OnceGpuWorkIsScheduled;
#else
    return FrameHandOff::OnceGpuWorkIsFinished;
#endif
}

}
