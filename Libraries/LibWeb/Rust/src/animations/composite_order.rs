/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The composite order of animations.
//! https://www.w3.org/TR/web-animations-1/#animation-composite-order

use super::animation::{AnimationKind, AnimationRecord, OwningElement};
use crate::css::selector::{PseudoElementType, pseudo_element_from_ffi};
use std::cmp::Ordering;
use std::ffi::c_void;
use std::ptr::NonNull;

/// The classes of animations, in their composite order.
/// https://www.w3.org/TR/css-animations-2/#animation-composite-order
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum AnimationClass {
    Smil,
    CssAnimationWithOwningElement,
    CssTransition,
    CssAnimationWithoutOwningElement,
    /// An animation without a specific animation class.
    None,
}

fn animation_class(animation: &AnimationRecord) -> AnimationClass {
    match animation.kind() {
        AnimationKind::Smil { .. } => AnimationClass::Smil,
        AnimationKind::CssAnimation(_) if animation.owning_element().is_some() => {
            AnimationClass::CssAnimationWithOwningElement
        }
        AnimationKind::CssAnimation(_) => AnimationClass::CssAnimationWithoutOwningElement,
        AnimationKind::CssTransition(_) => AnimationClass::CssTransition,
        AnimationKind::Script => AnimationClass::None,
    }
}

/// Compares two of the host's `DOM::Element`s in tree order.
pub(crate) trait TreeOrder {
    fn compare(&mut self, first: NonNull<c_void>, second: NonNull<c_void>) -> Ordering;
}

impl<F: FnMut(NonNull<c_void>, NonNull<c_void>) -> Ordering> TreeOrder for F {
    fn compare(&mut self, first: NonNull<c_void>, second: NonNull<c_void>) -> Ordering {
        self(first, second)
    }
}

/// https://www.w3.org/TR/web-animations-1/#animation-composite-order
/// The relative composite order of the animations `a` and `b`, which `tree_order` resolves where it depends on the
/// order of elements in the document.
pub(crate) fn composite_order(a: &AnimationRecord, b: &AnimationRecord, tree_order: &mut impl TreeOrder) -> Ordering {
    // The relative composite order of any two keyframe effects A and B within an effect stack is established by
    // comparing their properties as follows:

    // 1. Sort A and B by applying the following conditions in turn until the order is resolved,

    //    1. If A and B’s associated animations differ by class, sort by any inter-class composite order as defined
    //       for the corresponding classes.
    // From https://www.w3.org/TR/css-animations-2/#animation-composite-order:
    // "CSS Animations with an owning element have a later composite order than CSS Transitions but an earlier
    // composite order than animations without a specific animation class."
    let class = animation_class(a);
    let other_class = animation_class(b);
    if class != other_class {
        return class.cmp(&other_class);
    }

    //    2. If A and B are still not sorted, sort by any class-specific composite order defined by the common class of
    //       A and B’s associated animations.
    let order = class_specific_composite_order(a, b, tree_order);
    if order != Ordering::Equal {
        return order;
    }

    //    3. If A and B are still not sorted, sort by the position of their associated animations in the global
    //       animation list.
    a.global_animation_list_order().cmp(&b.global_animation_list_order())
}

fn class_specific_composite_order(
    a: &AnimationRecord,
    b: &AnimationRecord,
    tree_order: &mut impl TreeOrder,
) -> Ordering {
    match (a.kind(), b.kind()) {
        (AnimationKind::CssAnimation(a_state), AnimationKind::CssAnimation(b_state)) => {
            // https://www.w3.org/TR/css-animations-2/#animation-composite-order
            // Within the set of CSS Animations with an owning element, two animations A and B are sorted in composite
            // order (first to last) as follows:
            let (Some(owner), Some(other_owner)) = (a.owning_element(), b.owning_element()) else {
                // The composite order of CSS Animations without an owning element is based on their position in the
                // global animation list.
                return Ordering::Equal;
            };

            // 1. If the owning element of A and B differs, sort A and B by tree order of their corresponding owning
            //    elements. With regard to pseudo-elements, the sort order is as follows:
            //    - element
            //    - ::marker
            //    - ::before
            //    - any other pseudo-elements not mentioned specifically in this list, sorted in ascending order by the
            //      Unicode codepoints that make up each selector
            //    - ::after
            //    - element children
            if owner != other_owner {
                return owning_element_order(owner, other_owner, tree_order);
            }

            // 2. Otherwise, sort A and B based on their position in the computed value of the animation-name property
            //    of the (common) owning element.
            a_state.name_index.cmp(&b_state.name_index)
        }
        (AnimationKind::CssTransition(a_state), AnimationKind::CssTransition(b_state)) => {
            // https://drafts.csswg.org/css-transitions-2/#animation-composite-order
            // Within the set of CSS Transitions, two animations A and B are sorted in composite order (first to last)
            // as follows:
            match (a.owning_element(), b.owning_element()) {
                // 1. If neither A nor B has an owning element, sort based on their relative position in the global
                //    animation list.
                (None, None) => return a.global_animation_list_order().cmp(&b.global_animation_list_order()),
                // 2. Otherwise, if only one of A or B has an owning element, let the animation with an owning element
                //    sort first.
                (Some(_), None) => return Ordering::Less,
                (None, Some(_)) => return Ordering::Greater,
                // 3. Otherwise, if the owning element of A and B differs, sort A and B by tree order of their
                //    corresponding owning elements. With regard to pseudo-elements, the sort order is as follows:
                //    - element
                //    - ::marker
                //    - ::before
                //    - any other pseudo-elements not mentioned specifically in this list, sorted in ascending order by
                //      the Unicode codepoints that make up each selector
                //    - ::after
                //    - element children
                (Some(owner), Some(other_owner)) if owner != other_owner => {
                    return owning_element_order(owner, other_owner, tree_order);
                }
                (Some(_), Some(_)) => {}
            }

            // 4. Otherwise, if A and B have different transition generation values, sort by their corresponding
            //    transition generation in ascending order.
            if a_state.generation != b_state.generation {
                return a_state.generation.cmp(&b_state.generation);
            }

            // 5. Otherwise, sort A and B in ascending order by the Unicode codepoints that make up the expanded
            //    transition property name of each transition (i.e. without attempting case conversion and such that
            //    ‘-moz-column-width’ sorts before ‘column-width’).
            a_state.property_name().cmp(&b_state.property_name())
        }
        (
            AnimationKind::Smil {
                begin_time,
                animation_element,
                ..
            },
            AnimationKind::Smil {
                begin_time: other_begin_time,
                animation_element: other_animation_element,
                ..
            },
        ) => {
            // https://www.w3.org/TR/smil-animation/#AnimationSandwichModel
            // Within an OM, animations are prioritized according to when each begins. The animation first begun has
            // lowest priority and the most recently begun animation has highest priority. When two animations start at
            // the same moment in time, the activation order is resolved as follows:
            // ...
            // If two animations share no time dependency relationship (e.g., neither is defined relative to the other,
            // even indirectly) the element that appears first in the document has lower priority.
            if begin_time != other_begin_time {
                return begin_time.total_cmp(other_begin_time);
            }
            // NB: Begin times only come from offsets and DOM method calls for now, so no animation is defined relative
            //     to another.
            if animation_element == other_animation_element {
                return Ordering::Equal;
            }
            tree_order.compare(*animation_element, *other_animation_element)
        }
        _ => Ordering::Equal,
    }
}

/// The order of two different owning elements: the tree order of their elements, and for the element itself and its
/// pseudo-elements, the order the CSS animations and transitions composite order gives them.
fn owning_element_order(owner: OwningElement, other_owner: OwningElement, tree_order: &mut impl TreeOrder) -> Ordering {
    // NB: An element's pseudo-elements sort with it, so before its children.
    if owner.element != other_owner.element {
        return tree_order.compare(owner.element, other_owner.element);
    }
    let pseudo_element = owner.pseudo_element.map(pseudo_element_from_ffi);
    let other_pseudo_element = other_owner.pseudo_element.map(pseudo_element_from_ffi);
    pseudo_element_rank(pseudo_element)
        .cmp(&pseudo_element_rank(other_pseudo_element))
        .then_with(|| {
            let name = |pseudo_element: Option<PseudoElementType>| pseudo_element.map_or("", PseudoElementType::name);
            name(pseudo_element).cmp(name(other_pseudo_element))
        })
}

fn pseudo_element_rank(pseudo_element: Option<PseudoElementType>) -> u8 {
    match pseudo_element {
        None => 0,
        Some(PseudoElementType::Marker) => 1,
        Some(PseudoElementType::Before) => 2,
        Some(PseudoElementType::After) => 4,
        Some(_) => 3,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::animations::animation::{AnimationHandle, CssTransitionState, TransitionPhase};
    use crate::css::retained_fly_string::RetainedUtf16FlyString;
    use crate::css::style_value::RetainedStyleValueData;

    fn element(address: usize) -> NonNull<c_void> {
        NonNull::new(address as *mut c_void).unwrap()
    }

    fn by_address(first: NonNull<c_void>, second: NonNull<c_void>) -> Ordering {
        first.cmp(&second)
    }

    fn transition(owner: Option<OwningElement>, generation: u64, property_id: u16) -> AnimationHandle {
        let animation = AnimationHandle::new(AnimationKind::CssTransition(CssTransitionState {
            custom_property_name: RetainedUtf16FlyString::none(),
            generation,
            start_time: 0.0,
            start_value: RetainedStyleValueData::none(),
            end_value: RetainedStyleValueData::none(),
            reversing_adjusted_start_value: RetainedStyleValueData::none(),
            reversing_shortening_factor: 1.0,
            property_id,
            previous_phase: TransitionPhase::Before,
            provisional: false,
            initial_ready_time: None,
            adopted_from_lane: false,
            effect_started_with: None,
        }));
        animation.borrow_mut().set_owning_element(owner);
        animation
    }

    fn order(a: &AnimationHandle, b: &AnimationHandle) -> Ordering {
        composite_order(&a.borrow(), &b.borrow(), &mut by_address)
    }

    #[test]
    fn classes_sort_before_script_animations() {
        let script = AnimationHandle::new(AnimationKind::Script);
        let smil = AnimationHandle::new(AnimationKind::Smil {
            begin_time: 0.0,
            animation_element: element(8),
            holders: 0,
        });
        let transition = transition(None, 0, 1);
        assert_eq!(order(&script, &smil), Ordering::Greater);
        assert_eq!(order(&smil, &transition), Ordering::Less);
        assert_eq!(order(&transition, &script), Ordering::Less);
    }

    #[test]
    fn transitions_sort_by_owner_then_generation() {
        let owner = |address, pseudo_element| {
            Some(OwningElement {
                element: element(address),
                pseudo_element,
            })
        };
        let later_element = transition(owner(16, None), 0, 1);
        let earlier_element = transition(owner(8, None), 1, 1);
        assert_eq!(order(&later_element, &earlier_element), Ordering::Greater);

        let after = transition(owner(8, Some(PseudoElementType::After as u8)), 0, 1);
        let before = transition(owner(8, Some(PseudoElementType::Before as u8)), 0, 1);
        assert_eq!(order(&after, &before), Ordering::Greater);
        assert_eq!(order(&earlier_element, &before), Ordering::Less);

        let later_generation = transition(owner(8, None), 2, 1);
        assert_eq!(order(&earlier_element, &later_generation), Ordering::Less);

        let unowned = transition(None, 0, 1);
        assert_eq!(order(&unowned, &earlier_element), Ordering::Greater);
    }
}
