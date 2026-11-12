/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <Compositor/CompositorCoreRustFFI.h>
#include <Compositor/FrameInputs.h>
#include <Compositor/RemoteSublayer.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibGfx/Rect.h>

namespace Compositor {

using LiveMedia = RustFFI::FfiLiveMedia;
using LayerKind = RustFFI::FfiLayerKind;

bool same_media(ReadonlySpan<LiveMedia>, ReadonlySpan<LiveMedia>);

class LayerPlan {
    AK_MAKE_NONCOPYABLE(LayerPlan);
    AK_MAKE_NONMOVABLE(LayerPlan);

public:
    AK_ALLOC_WITH_KMALLOC;

    static OwnPtr<LayerPlan> build(FrameInputs const&, Compositing::DisplayList const&, Gfx::IntRect viewport_rect, ReadonlySpan<LiveMedia> active_media);
    ~LayerPlan();

    using WindowServerPlacement = Compositor::WindowServerPlacement;

    struct Layer {
        LayerKind kind;
        LiveMedia media;
        Gfx::IntRect rect;
        ReadonlySpan<Compositing::RustFFI::RecordSpan> record_spans;
        Optional<WindowServerPlacement> window_server_placement;
    };

    ReadonlySpan<Layer> layers() const { return m_layers; }
    ReadonlySpan<u32> media_record_offsets() const { return m_media_record_offsets; }

    Gfx::IntRect rect_of_media_layers_showing(ReadonlySpan<Compositing::CanvasId>, ReadonlySpan<Compositing::VideoSinkResourceId>) const;

    bool has_same_layers_as(LayerPlan const&) const;

    RustFFI::ContextLayerPlan const* rust_plan() const { return m_plan; }

private:
    explicit LayerPlan(RustFFI::ContextLayerPlan*);

    RustFFI::ContextLayerPlan* m_plan { nullptr };
    Vector<Layer> m_layers;
    Vector<u32> m_media_record_offsets;
};

class MediaActivity {
public:
    static constexpr AK::Duration longest_time_between_updates_that_activate_media = AK::Duration::from_seconds(1);
    static constexpr AK::Duration time_media_stays_active_after_an_update = AK::Duration::from_seconds(3);

    struct Update {
        LiveMedia media;
        u64 content_version { 0 };
    };

    void note_updates(ReadonlySpan<Update>, MonotonicTime now);
    Vector<LiveMedia> active_media(MonotonicTime now) const;

private:
    struct Updates {
        LiveMedia media;
        u64 content_version { 0 };
        MonotonicTime last_update;
        bool active { false };
    };

    void note_update(Update const&, MonotonicTime now);

    Vector<Updates> m_updates;
};

}
