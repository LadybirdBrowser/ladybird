/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Tuple.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/ViewTransitionPrototype.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Filter.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/CSS/Transformation.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::ViewTransition {

// https://drafts.csswg.org/css-view-transitions-1/#named-view-transition-pseudo
class NamedViewTransitionPseudoElement
    : public DOM::PseudoElementTreeNode {
    GC_CELL(NamedViewTransitionPseudoElement, DOM::PseudoElementTreeNode);
    GC_DECLARE_ALLOCATOR(NamedViewTransitionPseudoElement);

    NamedViewTransitionPseudoElement(CSS::PseudoElement, FlyString);

    CSS::PseudoElement m_type;

    // Several of the view transition pseudo-elements are named view transition pseudo-elements, which are
    // functional tree-abiding view transition pseudo-elements associated with a view transition name.
    FlyString m_view_transition_name;
};

// https://drafts.csswg.org/css-view-transitions-1/#::view-transition-old
// https://drafts.csswg.org/css-view-transitions-1/#::view-transition-new
class ReplacedNamedViewTransitionPseudoElement
    : public NamedViewTransitionPseudoElement {
    GC_CELL(ReplacedNamedViewTransitionPseudoElement, NamedViewTransitionPseudoElement);
    GC_DECLARE_ALLOCATOR(ReplacedNamedViewTransitionPseudoElement);

    ReplacedNamedViewTransitionPseudoElement(CSS::PseudoElement, FlyString, RefPtr<Gfx::ImmutableBitmap>);

    RefPtr<Gfx::ImmutableBitmap> m_content;
};

// https://drafts.csswg.org/css-view-transitions-1/#captured-element
struct CapturedElement : public JS::Cell {
    GC_CELL(CapturedElement, JS::Cell)
    GC_DECLARE_ALLOCATOR(CapturedElement);

    RefPtr<Gfx::ImmutableBitmap> old_image {};
    CSSPixels old_width = 0;
    CSSPixels old_height = 0;
    // FIXME: Make this an identity transform function by default.
    CSS::Transformation old_transform = CSS::Transformation(CSS::TransformFunction::Translate, Vector<CSS::TransformValue>());
    Optional<CSS::WritingMode> old_writing_mode {};
    Optional<CSS::Direction> old_direction {};
    // FIXME: old_text_orientation
    Optional<CSS::MixBlendMode> old_mix_blend_mode {};
    CSS::Filter old_backdrop_filter {};
    Optional<CSS::PreferredColorScheme> old_color_scheme {};
    GC::Ptr<DOM::Element> new_element {};

    GC::Ptr<CSS::CSSKeyframesRule> group_keyframes {};
    GC::Ptr<CSS::CSSStyleRule> group_animation_name_rule {};
    GC::Ptr<CSS::CSSStyleRule> group_styles_rule {};
    GC::Ptr<CSS::CSSStyleRule> image_pair_isolation_rule {};
    GC::Ptr<CSS::CSSStyleRule> image_animation_name_rule {};

private:
    virtual void visit_edges(JS::Cell::Visitor&) override;
};

// https://drafts.csswg.org/css-view-transitions-1/#callbackdef-viewtransitionupdatecallback
using ViewTransitionUpdateCallback = GC::Ptr<WebIDL::CallbackType>;

class ViewTransition final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(ViewTransition, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(ViewTransition);

public:
    static GC::Ref<ViewTransition> create(JS::Realm&);
    virtual ~ViewTransition() override = default;

    // https://drafts.csswg.org/css-view-transitions-1/#dom-viewtransition-updatecallbackdone
    GC::Ref<WebIDL::Promise> update_callback_done() const { return m_update_callback_done_promise; }
    // https://drafts.csswg.org/css-view-transitions-1/#dom-viewtransition-ready
    GC::Ref<WebIDL::Promise> ready() const { return m_ready_promise; }
    // https://drafts.csswg.org/css-view-transitions-1/#dom-viewtransition-finished
    GC::Ref<WebIDL::Promise> finished() const { return m_finished_promise; }
    // https://drafts.csswg.org/css-view-transitions-1/#dom-viewtransition-skiptransition
    void skip_transition();

    // https://drafts.csswg.org/css-view-transitions-1/#setup-view-transition
    void setup_view_transition();

    // https://drafts.csswg.org/css-view-transitions-1/#activate-view-transition
    void activate_view_transition();

    // https://drafts.csswg.org/css-view-transitions-1/#capture-the-old-state
    ErrorOr<void> capture_the_old_state();

    // https://drafts.csswg.org/css-view-transitions-1/#capture-the-new-state
    ErrorOr<void> capture_the_new_state();

    // https://drafts.csswg.org/css-view-transitions-1/#setup-transition-pseudo-elements
    void setup_transition_pseudo_elements();

    // https://drafts.csswg.org/css-view-transitions-1/#call-the-update-callback
    void call_the_update_callback();

    // https://drafts.csswg.org/css-view-transitions-1/#schedule-the-update-callback
    void schedule_the_update_callback();

    // https://drafts.csswg.org/css-view-transitions-1/#skip-the-view-transition
    void skip_the_view_transition(JS::Value reason);

    // https://drafts.csswg.org/css-view-transitions-1/#handle-transition-frame
    void handle_transition_frame();

    // https://drafts.csswg.org/css-view-transitions-1/#update-pseudo-element-styles
    ErrorOr<void> update_pseudo_element_styles();

    // https://drafts.csswg.org/css-view-transitions-1/#clear-view-transition
    void clear_view_transition();

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-phase
    enum class Phase : u8 {
        PendingCapture,
        UpdateCallbackCalled,
        Animating,
        Done,
    };
    Phase phase() const { return m_phase; }
    void set_update_callback(ViewTransitionUpdateCallback callback) { m_update_callback = callback; }

private:
    ViewTransition(JS::Realm&, GC::Ref<WebIDL::Promise>, GC::Ref<WebIDL::Promise>, GC::Ref<WebIDL::Promise>);
    virtual void initialize(JS::Realm&) override;

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-named-elements
    HashMap<FlyString, GC::Ptr<CapturedElement>> m_named_elements = {};

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-phase
    Phase m_phase = Phase::PendingCapture;

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-update-callback
    ViewTransitionUpdateCallback m_update_callback = {};

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-ready-promise
    GC::Ref<WebIDL::Promise> m_ready_promise;

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-update-callback-done-promise
    GC::Ref<WebIDL::Promise> m_update_callback_done_promise;

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-finished-promise
    GC::Ref<WebIDL::Promise> m_finished_promise;

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-transition-root-pseudo-element
    GC::Ref<DOM::PseudoElementTreeNode> m_transition_root_pseudo_element;

    // https://drafts.csswg.org/css-view-transitions-1/#viewtransition-initial-snapshot-containing-block-size
    Optional<CSSPixelSize> m_initial_snapshot_containing_block_size;
};

}
