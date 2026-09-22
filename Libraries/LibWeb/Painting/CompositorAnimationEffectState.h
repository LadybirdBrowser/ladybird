/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Layout::RustFFI {

enum class FfiVisualAnimationTargetKind : u8;

}

namespace Web::Painting {

// The keyframes of an effect as the compositor animation builder reads them: which properties each
// keyframe gives a value, the easing descriptors with the storage they borrow, and the keyframe set
// entries the builder resolves values from by index, against the effect's target. An update pass
// builds them once for each effect it asks about.
class WEB_API CompositorAnimationKeyframes {
    AK_MAKE_NONCOPYABLE(CompositorAnimationKeyframes);
    AK_MAKE_NONMOVABLE(CompositorAnimationKeyframes);

public:
    AK_ALLOC_WITH_KMALLOC;

    CompositorAnimationKeyframes(Animations::KeyframeEffect const&, Animations::Animation const&, DOM::AbstractElement target);
    ~CompositorAnimationKeyframes();

    // Whether the transforms the effect animates keep the axes in place: it does not rotate, and
    // its transform keyframes only translate and scale.
    bool transform_preserves_axes(Layout::Node const&) const;
    // Whether the effect targets only the transform property and only ever translates horizontally.
    bool only_translates_horizontally(Layout::Node const&) const;

    struct Data;

private:
    friend class CompositorAnimationEffectState;

    NonnullOwnPtr<Data> m_data;
};

// What a keyframe effect has built for the compositor, kept in Rust: the values lowered from its
// keyframes, the animations built in the current update pass, and the ones it published last.
class WEB_API CompositorAnimationEffectState {
    AK_MAKE_NONCOPYABLE(CompositorAnimationEffectState);
    AK_MAKE_NONMOVABLE(CompositorAnimationEffectState);

public:
    AK_ALLOC_WITH_KMALLOC;

    CompositorAnimationEffectState();
    ~CompositorAnimationEffectState();

    struct BuildOutcome {
        // The animation is built and waits with the effect's other pending animations.
        bool built { false };
        // The animation was valid but the target owns no node of the kind it drives yet.
        bool missing_visual_context_node { false };
        // Whether a transform animation's keyframes only ever translate horizontally, once known.
        Optional<bool> only_translates_horizontally;
    };

    // The instant the animation is anchored at, as the monotonic clock and the animation's own
    // local time; the compositor derives every later local time from it.
    struct TimingAnchor {
        double monotonic_time_ms { 0 };
        double local_time_ms { 0 };
    };

    // Builds the animation of one target kind from the effect's keyframes and timing for the nodes
    // of the target's box, and keeps it pending. The builder in Rust lowers and validates the
    // keyframes.
    BuildOutcome build(CompositorAnimationKeyframes const&, Layout::Node const&, Compositing::RustFFI::FfiVisualAnimationTargetKind, TimingAnchor);
    void discard_pending(Compositing::RustFFI::FfiVisualAnimationTargetKind);
    bool has_pending() const;
    void clear_pending();

    enum class ReuseRetainedTimingAnchors : u8 {
        No,
        Yes,
    };
    // The pending animations become the retained ones, and copies join the document's list for the
    // current update pass.
    void publish_pending(DOM::Document&, ReuseRetainedTimingAnchors);
    bool has_retained() const;
    void clear_retained();

    // The keyframes changed: everything lowered from them is stale.
    void reset();

private:
    void* m_handle { nullptr };
};

}
