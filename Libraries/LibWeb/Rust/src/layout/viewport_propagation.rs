/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Propagation of the root element's and its body's style to the viewport: the principal
//! writing mode and the viewport's overflow. The host reads the inputs from the elements' own
//! style records rather than from their boxes, since the previous pass rewrote the boxes'
//! values and a `display: none` body has style but no box, and applies the results through
//! the boxes' style setters outside any layout pass.

use crate::css::css_enums::overflow;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use std::ffi::c_void;

/// What the host knows about the document element and its first HTML body child element.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiViewportPropagationFacts {
    /// Invalid when there is no document element or it has no box.
    pub root_layout_node: NodeSlotId,
    pub root_is_html_html_element: bool,
    pub root_overflow_x: u8,
    pub root_overflow_y: u8,
    pub root_writing_mode: u8,
    pub root_direction: u8,
    /// Any of size, inline-size, layout, style or paint containment.
    pub root_has_containment: bool,
    /// The root element has an HTML body child element with computed style.
    pub has_styled_body: bool,
    /// Invalid when the body has no box.
    pub body_layout_node: NodeSlotId,
    pub body_display_is_none: bool,
    pub body_overflow_x: u8,
    pub body_overflow_y: u8,
    pub body_writing_mode: u8,
    pub body_direction: u8,
    pub body_has_containment: bool,
}

/// Each callback receives a layout node shell and the values to set on its computed style.
#[repr(C)]
pub struct FfiViewportPropagationApplyCallbacks {
    pub context: *mut c_void,
    pub apply_overflow: unsafe extern "C" fn(*mut c_void, *mut c_void, u8, u8),
    pub apply_writing_mode_and_direction: unsafe extern "C" fn(*mut c_void, *mut c_void, u8, u8),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PropagatedViewportStyles {
    pub(crate) writing_mode: u8,
    pub(crate) direction: u8,
    pub(crate) viewport_overflow: (u8, u8),
    pub(crate) root_overflow: (u8, u8),
    /// Present when the body has a box that takes part in overflow propagation.
    pub(crate) body_overflow: Option<(u8, u8)>,
}

// https://drafts.csswg.org/css-overflow-3/#overflow-propagation
// If 'visible' is applied to the viewport, it must be interpreted as 'auto'. If 'clip' is applied
// to the viewport, it must be interpreted as 'hidden'.
fn overflow_as_applied_to_viewport(value: u8) -> u8 {
    match value {
        overflow::VISIBLE => overflow::AUTO,
        overflow::CLIP => overflow::HIDDEN,
        other => other,
    }
}

/// `None` when the document element has no box; the viewport then only gets `overflow: auto`.
pub(crate) fn decide_viewport_propagation(facts: &FfiViewportPropagationFacts) -> Option<PropagatedViewportStyles> {
    if facts.root_layout_node.is_invalid() {
        return None;
    }
    let body_is_styled = facts.has_styled_body;
    let body_is_rendered = body_is_styled && !facts.body_display_is_none;

    // https://drafts.csswg.org/css-writing-modes-4/#principal-flow
    // The principal writing mode of the document is determined by the used writing-mode,
    // direction, and text-orientation values of the root element. As a special case for handling
    // HTML documents, if the root element has a body child element whose display value is not
    // none, the used value of the writing-mode and direction properties on the root element are
    // taken from the computed writing-mode and direction of the first such child element instead
    // of from the root element's own values.
    // NOTE: Using containment disables this special handling of the HTML body element.
    let mut writing_mode = facts.root_writing_mode;
    let mut direction = facts.root_direction;
    let propagation_is_disabled_by_containment =
        facts.root_has_containment || (body_is_styled && facts.body_has_containment);
    if facts.root_is_html_html_element && !propagation_is_disabled_by_containment && body_is_rendered {
        writing_mode = facts.body_writing_mode;
        direction = facts.body_direction;
    }

    // https://drafts.csswg.org/css-contain-2/#contain-property
    // Additionally, when any containments are active on either the HTML <html> or <body>
    // elements, propagation of properties from the <body> element to the initial containing
    // block, the viewport, or the canvas background, is disabled. Notably, this affects:
    // - 'overflow' and its longhands (see CSS Overflow 3 § 3.3 Overflow Viewport Propagation)
    let body_can_propagate_overflow = body_is_rendered && !facts.body_layout_node.is_invalid();
    let body_propagation_is_disabled_by_containment = (facts.root_is_html_html_element && facts.root_has_containment)
        || (body_is_styled && facts.body_has_containment);

    // https://drafts.csswg.org/css-overflow-3/#overflow-propagation
    // UAs must apply the overflow-* values set on the root element to the viewport when the
    // root element's display value is not none. However, when the root element is an [HTML]
    // html element (including XML syntax for HTML) whose overflow value is visible (in both
    // axes), and that element has as a child a body element whose display value is also not
    // none, user agents must instead apply the overflow-* values of the first such child element
    // to the viewport.
    let root_overflow = (facts.root_overflow_x, facts.root_overflow_y);
    let body_overflow = (facts.body_overflow_x, facts.body_overflow_y);
    let body_is_overflow_origin = facts.root_is_html_html_element
        && !body_propagation_is_disabled_by_containment
        && root_overflow == (overflow::VISIBLE, overflow::VISIBLE)
        && body_can_propagate_overflow;
    let origin_overflow = if body_is_overflow_origin {
        body_overflow
    } else {
        root_overflow
    };

    // The element from which the value is propagated must then have a used overflow value of
    // visible.
    // FIXME: Apply this to the used values, not the computed ones.
    let visible = (overflow::VISIBLE, overflow::VISIBLE);
    Some(PropagatedViewportStyles {
        writing_mode,
        direction,
        viewport_overflow: (
            overflow_as_applied_to_viewport(origin_overflow.0),
            overflow_as_applied_to_viewport(origin_overflow.1),
        ),
        root_overflow: if body_is_overflow_origin {
            root_overflow
        } else {
            visible
        },
        body_overflow: body_can_propagate_overflow.then_some(if body_is_overflow_origin {
            visible
        } else {
            body_overflow
        }),
    })
}

/// Applies the propagation in the order the boxes were always rewritten in: the root's writing
/// mode and direction, the viewport's, then the viewport's, the root's and the body's overflow.
/// Every box receives its final values exactly once, so a steady-state pass leaves every style
/// record untouched instead of oscillating values within the pass.
///
/// # Safety
///
/// The arena, `facts` and `callbacks` must remain valid for the duration of the call, `viewport`
/// and the boxes named by the facts must be live, and the call must be made outside any layout
/// pass: the apply callbacks replace computed values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_propagate_root_styles_to_viewport(
    arena: *mut c_void,
    viewport: NodeSlotId,
    facts: *const FfiViewportPropagationFacts,
    callbacks: *const FfiViewportPropagationApplyCallbacks,
) {
    assert!(!viewport.is_invalid());
    assert!(!facts.is_null());
    assert!(!callbacks.is_null());
    // SAFETY: The C++ caller keeps the arena, the facts and the callback table alive for this
    // synchronous call.
    let arena = unsafe { LayoutNodeArena::from_handle(arena) };
    let facts = unsafe { &*facts };
    let callbacks = unsafe { &*callbacks };
    let apply_overflow = |node: NodeSlotId, (overflow_x, overflow_y): (u8, u8)| {
        // SAFETY: The node is live, and the host applies the values synchronously.
        unsafe { (callbacks.apply_overflow)(callbacks.context, arena.node_shell(node), overflow_x, overflow_y) };
    };
    let apply_writing_mode_and_direction = |node: NodeSlotId, writing_mode: u8, direction: u8| {
        // SAFETY: As above.
        unsafe {
            (callbacks.apply_writing_mode_and_direction)(
                callbacks.context,
                arena.node_shell(node),
                writing_mode,
                direction,
            );
        }
    };

    let Some(styles) = decide_viewport_propagation(facts) else {
        apply_overflow(viewport, (overflow::AUTO, overflow::AUTO));
        return;
    };
    let root = facts.root_layout_node;
    apply_writing_mode_and_direction(root, styles.writing_mode, styles.direction);
    // https://drafts.csswg.org/css-writing-modes-4/#icb
    // The principal writing mode is propagated to the initial containing block and to the
    // viewport, thereby affecting the layout of the root element and the scrolling direction of
    // the viewport.
    apply_writing_mode_and_direction(viewport, styles.writing_mode, styles.direction);
    apply_overflow(viewport, styles.viewport_overflow);
    apply_overflow(root, styles.root_overflow);
    if let Some(body_overflow) = styles.body_overflow {
        apply_overflow(facts.body_layout_node, body_overflow);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_enums::{direction, writing_mode};

    fn html_document_facts() -> FfiViewportPropagationFacts {
        FfiViewportPropagationFacts {
            root_layout_node: NodeSlotId::new(1, 1),
            root_is_html_html_element: true,
            root_overflow_x: overflow::VISIBLE,
            root_overflow_y: overflow::VISIBLE,
            root_writing_mode: writing_mode::HORIZONTAL_TB,
            root_direction: direction::LTR,
            root_has_containment: false,
            has_styled_body: true,
            body_layout_node: NodeSlotId::new(2, 1),
            body_display_is_none: false,
            body_overflow_x: overflow::SCROLL,
            body_overflow_y: overflow::HIDDEN,
            body_writing_mode: writing_mode::VERTICAL_RL,
            body_direction: direction::RTL,
            body_has_containment: false,
        }
    }

    const VISIBLE: (u8, u8) = (overflow::VISIBLE, overflow::VISIBLE);

    #[test]
    fn a_visible_html_root_hands_propagation_to_its_body() {
        let styles = decide_viewport_propagation(&html_document_facts()).unwrap();
        assert_eq!(styles.writing_mode, writing_mode::VERTICAL_RL);
        assert_eq!(styles.direction, direction::RTL);
        assert_eq!(styles.viewport_overflow, (overflow::SCROLL, overflow::HIDDEN));
        assert_eq!(styles.root_overflow, VISIBLE);
        assert_eq!(styles.body_overflow, Some(VISIBLE));
    }

    #[test]
    fn a_root_with_its_own_overflow_stays_the_origin_and_maps_the_viewport_keywords() {
        let mut facts = html_document_facts();
        facts.root_overflow_x = overflow::CLIP;
        let styles = decide_viewport_propagation(&facts).unwrap();
        assert_eq!(styles.viewport_overflow, (overflow::HIDDEN, overflow::AUTO));
        assert_eq!(styles.root_overflow, VISIBLE);
        assert_eq!(styles.body_overflow, Some((overflow::SCROLL, overflow::HIDDEN)));
        assert_eq!(styles.writing_mode, writing_mode::VERTICAL_RL);
    }

    #[test]
    fn a_body_without_a_box_still_supplies_the_writing_mode_but_no_overflow() {
        let mut facts = html_document_facts();
        facts.body_layout_node = NodeSlotId::INVALID;
        let styles = decide_viewport_propagation(&facts).unwrap();
        assert_eq!(styles.writing_mode, writing_mode::VERTICAL_RL);
        assert_eq!(styles.viewport_overflow, (overflow::AUTO, overflow::AUTO));
        assert_eq!(styles.root_overflow, VISIBLE);
        assert_eq!(styles.body_overflow, None);
    }

    #[test]
    fn a_display_none_body_takes_no_part_in_either_propagation() {
        let mut facts = html_document_facts();
        facts.body_layout_node = NodeSlotId::INVALID;
        facts.body_display_is_none = true;
        let styles = decide_viewport_propagation(&facts).unwrap();
        assert_eq!(styles.writing_mode, writing_mode::HORIZONTAL_TB);
        assert_eq!(styles.direction, direction::LTR);
        assert_eq!(styles.viewport_overflow, (overflow::AUTO, overflow::AUTO));
        assert_eq!(styles.body_overflow, None);
    }

    #[test]
    fn containment_on_either_element_disables_both_body_special_cases() {
        for containment_is_on_root in [true, false] {
            let mut facts = html_document_facts();
            facts.root_has_containment = containment_is_on_root;
            facts.body_has_containment = !containment_is_on_root;
            let styles = decide_viewport_propagation(&facts).unwrap();
            assert_eq!(styles.writing_mode, writing_mode::HORIZONTAL_TB);
            assert_eq!(styles.viewport_overflow, (overflow::AUTO, overflow::AUTO));
            assert_eq!(styles.root_overflow, VISIBLE);
            assert_eq!(styles.body_overflow, Some((overflow::SCROLL, overflow::HIDDEN)));
        }
    }

    #[test]
    fn a_root_that_is_not_an_html_element_ignores_its_body_child() {
        let mut facts = html_document_facts();
        facts.root_is_html_html_element = false;
        let styles = decide_viewport_propagation(&facts).unwrap();
        assert_eq!(styles.writing_mode, writing_mode::HORIZONTAL_TB);
        assert_eq!(styles.viewport_overflow, (overflow::AUTO, overflow::AUTO));
        assert_eq!(styles.root_overflow, VISIBLE);
        assert_eq!(styles.body_overflow, Some((overflow::SCROLL, overflow::HIDDEN)));
    }

    #[test]
    fn a_missing_root_box_leaves_only_the_viewport_default() {
        let mut facts = html_document_facts();
        facts.root_layout_node = NodeSlotId::INVALID;
        assert_eq!(decide_viewport_propagation(&facts), None);
    }
}
