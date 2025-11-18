/*
 * Copyright (c) 2025, Rocco Corsi <5201151+rcorsi@users.noreply.github.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Graphics.h>
#include <LibGfx/VulkanContext.h>
#include <LibWebView/Options.h>

namespace Gfx {

void init_graphics(WebView::ForceCpuPainting force_cpu_painting)
{
    if (force_cpu_painting == WebView::ForceCpuPainting::Yes) {
        dbgln("Falling back to CPU Backend painting");
        return;
    }

#ifdef USE_VULKAN
    Gfx::init_vulkan_context();
#endif
}

}
