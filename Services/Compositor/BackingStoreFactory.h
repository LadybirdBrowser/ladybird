/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <Compositor/PaintingSurface.h>
#include <Compositor/PresentableSurface.h>
#include <LibGfx/Forward.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/Size.h>

namespace Compositor {

// A store of the kind that the backing stores of a context in the Rust core asked for: the surface frames are rendered
// into, the surface the window server shows it as, the memory it shares with the presentation, and the image the
// presentation imports it from.
struct NewBackingStore {
    NonnullRefPtr<Gfx::PaintingSurface> surface;
    RefPtr<PresentableSurface> window_server_surface;
    OwnPtr<Gfx::SharedImageBuffer> shared_image_buffer;
    Optional<Gfx::SharedImage> shared_image;
};

// The layouts that the presentation can import shared GPU backing stores in. Any layout the GPU renders to goes while
// this is empty.
void set_presentation_drm_format_modifiers(Vector<u64>);

ErrorOr<NewBackingStore> create_backing_store(RustFFI::StoreKind, Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&);

// Whether something other than the compositor still holds the memory of a store, such as the window server, or the
// presentation that has yet to import it.
bool shared_image_buffer_is_in_use(Gfx::SharedImageBuffer const&);

}
