/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <Compositor/PresentableSurface.h>
#include <Compositor/RemoteLayer.h>
#include <Compositor/WindowServer.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Gfx {

class D3DSharedTexture;

}

namespace Compositor {

class DirectCompositionRemoteLayer;

class DirectCompositionSurface final : public WindowServerSurface {
public:
    // Over memory that the window server cannot read, whose content is copied into a swap chain to be shown.
    DirectCompositionSurface();
    // Over a texture that the window server reads as it is, as the content of a visual.
    explicit DirectCompositionSurface(NonnullRefPtr<Gfx::D3DSharedTexture>);
    virtual ~DirectCompositionSurface() override;

    virtual bool may_be_read_by_window_server() const override;

private:
    friend class DirectCompositionRemoteLayer;

    struct Impl;

    OwnPtr<Impl> m_impl;
};

class DirectCompositionWindowServer final : public WindowServer {
public:
    static RefPtr<DirectCompositionWindowServer> try_create();
    static ErrorOr<NonnullRefPtr<PresentableSurface>> try_create_presentable_surface_on_gpu(Gfx::IntSize, Gfx::SkiaBackendContext&);
    virtual ~DirectCompositionWindowServer() override;

    virtual OwnPtr<RemoteLayer> try_create_remote_layer(u64 remote_layer_id) override;
    virtual ErrorOr<NonnullRefPtr<PresentableSurface>> try_create_presentable_surface(Gfx::IntSize, RefPtr<Gfx::SkiaBackendContext> const&) override;
    virtual bool shows_tiles_of_painted_layers() const override;
    virtual FrameHandOff frame_hand_off() const override;
    virtual void hold_changes_on_screen() override;
    virtual void release_changes_held_off_screen() override;

private:
    friend class DirectCompositionRemoteLayer;

    struct Impl;

    explicit DirectCompositionWindowServer(NonnullOwnPtr<Impl>);

    void commit();

    NonnullOwnPtr<Impl> m_impl;
    Vector<DirectCompositionRemoteLayer*> m_layers;
    bool m_holds_changes_off_screen { false };
};

}
