/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/SkiaBackendContext.h>
#include <Compositor/WindowServer.h>
#include <stdlib.h>

#ifdef AK_OS_MACOS
#    include <Compositor/CoreAnimationWindowServer.h>
#elif defined(AK_OS_WINDOWS) && defined(USE_DIRECTX)
#    include <Compositor/DirectCompositionWindowServer.h>
#endif

namespace Compositor {

RefPtr<WindowServer> WindowServer::try_create_for_presentation()
{
#ifdef AK_OS_MACOS
    return CoreAnimationWindowServer::try_create();
#elif defined(AK_OS_WINDOWS) && defined(USE_DIRECTX)
    return DirectCompositionWindowServer::try_create();
#else
    return nullptr;
#endif
}

ErrorOr<RefPtr<PresentableSurface>> WindowServer::try_create_presentable_surface_for_sublayers(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    // NB: A surface larger than the GPU's largest texture can't be a sublayer of its own, so it's drawn into the frame
    //     from memory instead.
    if (!skia_backend_context.can_hold_texture_of_size(size))
        return nullptr;
#ifdef AK_OS_MACOS
    return RefPtr<PresentableSurface> { TRY(CoreAnimationWindowServer::try_create_presentable_surface_over_new_iosurface(size, skia_backend_context)) };
#elif defined(AK_OS_WINDOWS) && defined(USE_DIRECTX)
    return RefPtr<PresentableSurface> { TRY(DirectCompositionWindowServer::try_create_presentable_surface_on_gpu(size, skia_backend_context)) };
#else
    return nullptr;
#endif
}

bool WindowServer::showing_tiles_of_painted_layers_is_switched_off()
{
    static bool is_switched_off = getenv("LADYBIRD_WINDOW_SERVER_SHOWS_NO_TILES") != nullptr;
    return is_switched_off;
}

}
