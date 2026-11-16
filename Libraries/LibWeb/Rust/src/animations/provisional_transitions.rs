/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transition actions a style stabilization epoch decides, which stay provisional until the epoch commits.
//! https://drafts.csswg.org/css-transitions/#starting

use super::animation::{AnimationHandle, HostCellKind};
use super::effect::AnimationEffectCell;
use super::element_animations::{AnimationOwnerSlot, ElementAnimations, TransitionProperty};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::transition::FfiTransitionActionKind;
use smallvec::SmallVec;
use std::collections::HashMap;
use std::ffi::c_void;
use std::ptr::NonNull;

/// The element, or one of its pseudo-elements, whose transitions a transition step decides.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct TransitionTarget {
    pub(crate) element: NonNull<c_void>,
    /// The host's `PseudoElement`, where the target is a pseudo-element.
    pub(crate) pseudo_element: Option<u8>,
}

/// What the epoch decided so far for the transitions of one property of one target.
pub(crate) struct ProvisionalTransitionState {
    pub(crate) target: TransitionTarget,
    /// The lists of the target's element, which the element owns for as long as it lives, and the slot of the target
    /// in them, where it can own transitions.
    pub(crate) animations: NonNull<ElementAnimations>,
    pub(crate) slot: Option<AnimationOwnerSlot>,
    pub(crate) property: TransitionProperty,
    /// The name of a custom property, which the state keys on by its raw fly string, kept alive with the state.
    _custom_name: RetainedUtf16FlyString,
    /// The transition of the property the target held as the epoch first decided over it.
    pub(crate) committed: Option<AnimationHandle>,
    /// The transition the latest decision started provisionally, if any.
    pub(crate) proposed: Option<AnimationHandle>,
    pub(crate) action: FfiTransitionActionKind,
    pub(crate) has_decision: bool,
}

/// The provisional transition states of a document's current style stabilization epoch, in the order the epoch first
/// decided over each.
#[derive(Default)]
pub(crate) struct ProvisionalTransitions {
    states: Vec<ProvisionalTransitionState>,
    by_property: HashMap<(TransitionTarget, TransitionProperty), usize>,
    by_target: HashMap<TransitionTarget, SmallVec<[usize; 2]>>,
}

impl ProvisionalTransitions {
    pub(crate) fn is_empty(&self) -> bool {
        self.states.is_empty()
    }

    pub(crate) fn has_states(&self, target: TransitionTarget) -> bool {
        self.by_target.contains_key(&target)
    }

    /// The indices of the states of `target`.
    pub(crate) fn indices_of(&self, target: TransitionTarget) -> SmallVec<[usize; 2]> {
        self.by_target.get(&target).cloned().unwrap_or_default()
    }

    pub(crate) fn state_mut(&mut self, index: usize) -> &mut ProvisionalTransitionState {
        &mut self.states[index]
    }

    pub(crate) fn state(&self, index: usize) -> &ProvisionalTransitionState {
        &self.states[index]
    }

    /// The index of the state of `property` of `target`, created with the transition of the property its lists
    /// `animations` hold at `slot` as the committed one where the epoch did not decide over it yet.
    ///
    /// # Safety
    /// `animations` must be the lists of the target's element.
    pub(crate) unsafe fn ensure(
        &mut self,
        target: TransitionTarget,
        animations: NonNull<ElementAnimations>,
        slot: Option<AnimationOwnerSlot>,
        property: TransitionProperty,
    ) -> usize {
        if let Some(&index) = self.by_property.get(&(target, property)) {
            return index;
        }
        let index = self.states.len();
        // SAFETY: Guaranteed by the caller.
        let committed = slot.and_then(|slot| unsafe { animations.as_ref() }.transition(slot, property).cloned());
        self.states.push(ProvisionalTransitionState {
            target,
            animations,
            slot,
            property,
            // SAFETY: The name is a live fly string the step decides over, or zero.
            _custom_name: unsafe { RetainedUtf16FlyString::from_borrowed_raw(property.custom_name) },
            committed,
            proposed: None,
            action: FfiTransitionActionKind::None,
            has_decision: false,
        });
        self.by_property.insert((target, property), index);
        self.by_target.entry(target).or_default().push(index);
        index
    }

    pub(crate) fn len(&self) -> usize {
        self.states.len()
    }

    /// The effect of the transition at `index` among those the epoch started provisionally for `target`, which the
    /// transition holds for as long as the epoch holds it, or null past the last.
    pub(crate) fn proposed_effect(&self, target: TransitionTarget, index: usize) -> *const AnimationEffectCell {
        self.by_target
            .get(&target)
            .into_iter()
            .flatten()
            .filter_map(|&state| self.states[state].proposed.as_ref())
            .filter_map(|transition| transition.borrow().effect().map(|effect| &raw const **effect))
            .nth(index)
            .unwrap_or(std::ptr::null())
    }

    /// Takes every state, as the epoch commits.
    pub(crate) fn take(&mut self) -> Vec<ProvisionalTransitionState> {
        self.by_property.clear();
        self.by_target.clear();
        std::mem::take(&mut self.states)
    }

    /// Calls `visit` with each GC cell of the host the states keep alive: the elements they decide over and what keeps
    /// alive what their transitions name.
    pub(crate) fn for_each_host_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        for state in &self.states {
            visit(state.target.element, HostCellKind::Element);
            for transition in state.committed.iter().chain(state.proposed.iter()) {
                transition.for_each_host_cell(&mut visit);
            }
        }
    }
}
