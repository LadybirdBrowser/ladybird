/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The animations of an element: those associated with it, the CSS animations and transitions it and its
//! pseudo-elements own, and the descriptions of the effects of each it last published to the style engine. The lists
//! publish what the style engine reads of them to the engine the host names as it changes them.
//! https://www.w3.org/TR/web-animations-1/#the-animatable-interface-mixin

use super::animation::{
    AnimationCell, AnimationHandle, AnimationRecord, HostCellKind, SmilHolder, WeakAnimationHandle,
};
use super::composite_order::{TreeOrder, composite_order};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::animations::CssDefinedAnimation;
use crate::css::style::effect_descriptions::EffectDescription;
use crate::css::style::engine_calls::EngineWrite;
use crate::css::style::tree::StyleNodeID;
use crate::heap_size::small_vec_bytes;
use crate::render_state::{ArenaChange, DocumentHost};
use smallvec::SmallVec;
use std::cmp::Ordering;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::Arc;

/// The element or one of its pseudo-elements, as one of the lists of the CSS animations and transitions it owns, and of
/// the effects that target it: 0 for the element, and the host's `PseudoElement` plus one for a pseudo-element.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AnimationOwnerSlot(pub u8);

/// The style engine that reads an element's lists, by the document host the host names it by and the element's identity
/// in it, as the host passes them each time it changes the lists, since the element's identity there changes over its
/// life and the lists follow the element from document to document.
#[derive(Clone, Copy)]
pub(crate) struct StyleEngineTarget<'a> {
    pub(crate) host: &'a DocumentHost,
    pub(crate) node: StyleNodeID,
}

/// The property a CSS transition runs: the host's `PropertyID` of a longhand, or the raw fly string of a custom
/// property's name where it is not zero.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) struct TransitionProperty {
    pub(crate) property_id: u16,
    pub(crate) custom_name: usize,
}

impl TransitionProperty {
    pub(crate) fn of(animation: &AnimationRecord) -> Self {
        let state = animation.css_transition();
        Self {
            property_id: state.property_id,
            custom_name: state.custom_property_name.raw(),
        }
    }
}

/// The CSS animations and transitions of one owner, and the effects that target it as the style engine has them.
struct OwnedAnimations {
    slot: AnimationOwnerSlot,
    /// The CSS animations, in the order of the owner's `animation-name`.
    css_animations: SmallVec<[AnimationHandle; 1]>,
    /// The running or completed transition of each property, which runs no other.
    transitions: SmallVec<[AnimationHandle; 1]>,
    /// The descriptions of the effects that target the owner, in composite order, as the host last published them to
    /// the style engine.
    published_effects: Box<[Arc<EffectDescription>]>,
}

impl OwnedAnimations {
    fn is_empty(&self) -> bool {
        self.css_animations.is_empty() && self.transitions.is_empty() && self.published_effects.is_empty()
    }
}

/// The animations of an element, which holds them strongly.
#[derive(Default)]
pub struct ElementAnimations {
    /// The animations whose effects target the element or one of its pseudo-elements.
    associated: SmallVec<[AnimationHandle; 1]>,
    /// The lists of the owners that own any CSS animation or transition.
    owners: SmallVec<[OwnedAnimations; 1]>,
    /// The animations disassociated from the element, which may still name it as their effect target: An animation
    /// that turns inert leaves `associated`, but its effect keeps targeting the element until it is revived.
    disassociated: SmallVec<[WeakAnimationHandle; 1]>,
    /// Whether `associated` is in composite order.
    is_sorted_by_composite_order: bool,
    /// Whether any owner has had a CSS animation since the element last canceled them all.
    has_css_defined_animations: bool,
}

impl Drop for ElementAnimations {
    fn drop(&mut self) {
        // NB: The element is finalized, and what else holds the records of its animations may only let go of them as
        //     the host sweeps it, after the element itself may be gone. So that no record names the element by then,
        //     nor is alive to the sets of animations by it, the records forget it here.
        for animation in &self.associated {
            animation.note_smil_holder(SmilHolder::TargetList, false);
            let effect = animation.borrow().effect().cloned();
            if let Some(effect) = effect {
                effect.borrow_mut().set_target_element(None);
            }
        }
        for owner in &self.owners {
            for animation in owner.css_animations.iter().chain(&owner.transitions) {
                animation.borrow_mut().set_owning_element(None);
            }
        }
    }
}

impl ElementAnimations {
    fn owner(&self, slot: AnimationOwnerSlot) -> Option<&OwnedAnimations> {
        self.owners.iter().find(|owner| owner.slot == slot)
    }

    fn ensure_owner(&mut self, slot: AnimationOwnerSlot) -> &mut OwnedAnimations {
        let index = match self.owners.iter().position(|owner| owner.slot == slot) {
            Some(index) => index,
            None => {
                self.owners.push(OwnedAnimations {
                    slot,
                    css_animations: SmallVec::new(),
                    transitions: SmallVec::new(),
                    published_effects: Box::default(),
                });
                self.owners.len() - 1
            }
        };
        &mut self.owners[index]
    }

    fn remove_empty_owners(&mut self) {
        self.owners.retain(|owner| !owner.is_empty());
    }

    /// Associates `animation` with the element, answering whether it was not associated yet.
    pub(crate) fn associate(&mut self, animation: AnimationHandle) -> bool {
        if self.associated.iter().any(|associated| associated.ptr_eq(&animation)) {
            return false;
        }
        self.disassociated
            .retain(|disassociated| disassociated.upgrade().is_some_and(|other| !other.ptr_eq(&animation)));
        animation.note_smil_holder(SmilHolder::TargetList, true);
        self.associated.push(animation);
        self.is_sorted_by_composite_order = false;
        true
    }

    /// Disassociates `animation` from the element, answering whether it was associated.
    pub(crate) fn disassociate(&mut self, animation: &AnimationCell) -> bool {
        self.is_sorted_by_composite_order = false;
        let Some(index) = self.associated.iter().position(|associated| associated.is(animation)) else {
            return false;
        };
        let animation = self.associated.remove(index);
        animation.note_smil_holder(SmilHolder::TargetList, false);
        self.disassociated
            .retain(|disassociated| disassociated.upgrade().is_some());
        self.disassociated.push(animation.downgrade());
        true
    }

    /// Makes the effects of the animations disassociated from `element`, whose lists these are, forget it as their
    /// target, as the element is finalized: An animation that outlives the element, for an event still pending on it,
    /// would otherwise name an element that is gone.
    pub(crate) fn forget_element_as_target_of_disassociated(&mut self, element: NonNull<c_void>) {
        for disassociated in self.disassociated.drain(..) {
            let Some(animation) = disassociated.upgrade() else {
                continue;
            };
            let effect = animation.borrow().effect().cloned();
            if let Some(effect) = effect
                && effect.borrow().target_element() == Some(element)
            {
                effect.borrow_mut().set_target_element(None);
            }
        }
    }

    pub(crate) fn associated(&self) -> &[AnimationHandle] {
        &self.associated
    }

    pub(crate) fn invalidate_composite_order(&mut self) {
        self.is_sorted_by_composite_order = false;
    }

    /// Sorts the associated animations in composite order, with `tree_order` comparing elements in tree order.
    pub(crate) fn sort_in_composite_order(&mut self, tree_order: &mut impl TreeOrder) {
        if self.is_sorted_by_composite_order {
            return;
        }
        // NB: An animation without an effect has no place in an effect stack, and sorts first by its position in the
        //     global animation list.
        self.associated.sort_by(|a, b| {
            let (a, b) = (a.borrow(), b.borrow());
            match (a.effect().is_some(), b.effect().is_some()) {
                (true, true) => composite_order(&a, &b, tree_order),
                (false, true) => Ordering::Less,
                (true, false) => Ordering::Greater,
                (false, false) => a.global_animation_list_order().cmp(&b.global_animation_list_order()),
            }
        });
        self.is_sorted_by_composite_order = true;
    }

    /// The CSS animations of the first owner after `slot`, or of the first owner without one, by the order of their
    /// slots, which is the element's and then each pseudo-element's in the order of the host's `PseudoElement`.
    pub(crate) fn css_animations_after(
        &self,
        slot: Option<AnimationOwnerSlot>,
    ) -> Option<(AnimationOwnerSlot, SmallVec<[AnimationHandle; 1]>)> {
        self.owners
            .iter()
            .filter(|owner| slot.is_none_or(|slot| owner.slot.0 > slot.0) && !owner.css_animations.is_empty())
            .min_by_key(|owner| owner.slot.0)
            .map(|owner| (owner.slot, owner.css_animations.clone()))
    }

    pub(crate) fn css_animations(&self, slot: AnimationOwnerSlot) -> &[AnimationHandle] {
        self.owner(slot).map_or(&[], |owner| &owner.css_animations)
    }

    /// Sets the CSS animations of the owner at `slot`, and publishes them to `engine`, if any, where the owner had or
    /// now has any.
    pub(crate) fn set_css_animations(
        &mut self,
        slot: AnimationOwnerSlot,
        animations: impl ExactSizeIterator<Item = AnimationHandle>,
        engine: Option<StyleEngineTarget<'_>>,
    ) {
        // NB: The flag says the element has animations to play or cancel, not that it has been through the step that
        //     would have registered some. It stays set once any list is non-empty, since the lists are per owner and
        //     this is one flag for all of them.
        if animations.len() != 0 {
            self.has_css_defined_animations = true;
        }
        let owner = self.ensure_owner(slot);
        let had_animations = !owner.css_animations.is_empty();
        let animations: SmallVec<[AnimationHandle; 1]> = animations.collect();
        // https://drafts.csswg.org/css-animations-1/#animations
        // Once an animation has started it continues until it ends or the animation-name is removed
        // NB: An animation removed from its owning element no longer follows its named timeline.
        for removed in &owner.css_animations {
            if !animations.iter().any(|animation| animation.ptr_eq(removed)) {
                removed.borrow_mut().css_animation_mut().named_timeline = RetainedUtf16FlyString::none();
            }
        }
        owner.css_animations = animations;
        let publishes = had_animations || !owner.css_animations.is_empty();
        self.remove_empty_owners();
        if publishes && let Some(engine) = engine {
            publish_css_defined_animations(engine, slot, self.css_animations(slot));
        }
    }

    pub(crate) fn has_css_animations_or_transitions(&self) -> bool {
        self.has_css_defined_animations || self.owners.iter().any(|owner| !owner.transitions.is_empty())
    }

    pub(crate) fn transitions(&self, slot: AnimationOwnerSlot) -> &[AnimationHandle] {
        self.owner(slot).map_or(&[], |owner| &owner.transitions)
    }

    /// The running or completed transition of `property` the owner at `slot` has, if any.
    pub(crate) fn transition(
        &self,
        slot: AnimationOwnerSlot,
        property: TransitionProperty,
    ) -> Option<&AnimationHandle> {
        self.transitions(slot)
            .iter()
            .find(|transition| TransitionProperty::of(&transition.borrow()) == property)
    }

    /// Gives the owner at `slot` the transition `transition` of a property it has no transition of.
    pub(crate) fn add_transition(&mut self, slot: AnimationOwnerSlot, transition: AnimationHandle) {
        let property = TransitionProperty::of(&transition.borrow());
        assert!(
            self.transition(slot, property).is_none(),
            "the owner has no transition of the property yet"
        );
        self.ensure_owner(slot).transitions.push(transition);
    }

    /// Removes the transition of `property` the owner at `slot` has, which is disassociated from its target once it is
    /// inert.
    pub(crate) fn remove_transition(&mut self, slot: AnimationOwnerSlot, property: TransitionProperty) {
        let owner = self.ensure_owner(slot);
        let index = owner
            .transitions
            .iter()
            .position(|transition| TransitionProperty::of(&transition.borrow()) == property)
            .expect("the owner has a transition of the property");
        let transition = owner.transitions.remove(index);
        self.remove_empty_owners();
        transition.borrow_mut().disassociation_from_target_pending = true;
    }

    /// Takes the CSS animations and transitions of the owner at `slot`, or of every owner where it names none, answering
    /// them, and publishes to `engine`, if any, that the owners that had CSS animations have none.
    pub(crate) fn take_css_animations_and_transitions(
        &mut self,
        slot: Option<AnimationOwnerSlot>,
        engine: Option<StyleEngineTarget<'_>>,
    ) -> Vec<AnimationHandle> {
        let mut animations = Vec::new();
        for owner in &mut self.owners {
            if slot.is_some_and(|slot| owner.slot != slot) {
                continue;
            }
            if !owner.css_animations.is_empty()
                && let Some(engine) = engine
            {
                publish_css_defined_animations(engine, owner.slot, &[]);
            }
            // NB: An animation removed from its owning element no longer follows its named timeline.
            for animation in std::mem::take(&mut owner.css_animations) {
                animation.borrow_mut().css_animation_mut().named_timeline = RetainedUtf16FlyString::none();
                animations.push(animation);
            }
            animations.extend(std::mem::take(&mut owner.transitions));
        }
        self.remove_empty_owners();
        if slot.is_none() {
            self.has_css_defined_animations = false;
        }
        animations
    }

    /// Notes that the host publishes `descriptions` as the effects that target the owner at `slot`, answering whether
    /// that publishes anything the style engine does not have: whether they differ, description by description, from
    /// what the host last published for it.
    pub(crate) fn publish_effect_descriptions(
        &mut self,
        slot: AnimationOwnerSlot,
        descriptions: &[Arc<EffectDescription>],
    ) -> bool {
        let published = self.owner(slot).map_or(&[][..], |owner| &owner.published_effects);
        if published.len() == descriptions.len()
            && published
                .iter()
                .zip(descriptions)
                .all(|(published, description)| Arc::ptr_eq(published, description))
        {
            return false;
        }
        self.ensure_owner(slot).published_effects = descriptions.into();
        self.remove_empty_owners();
        true
    }

    /// Forgets what the host published to the style engine, whose lists of the element's effects are those of the
    /// identity the element no longer has.
    pub(crate) fn forget_published_effect_descriptions(&mut self) {
        for owner in &mut self.owners {
            owner.published_effects = Box::default();
        }
        self.remove_empty_owners();
    }

    /// Calls `visit` with each GC cell of the host that keeps alive what an animation in the lists names.
    pub(crate) fn for_each_host_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        let owned = self
            .owners
            .iter()
            .flat_map(|owner| owner.css_animations.iter().chain(&owner.transitions));
        for animation in self.associated.iter().chain(owned) {
            animation.for_each_host_cell(&mut visit);
        }
    }

    /// The bytes of the lists, without the animations and the descriptions of their effects.
    pub(crate) fn memory_size(&self) -> usize {
        size_of::<Self>()
            + small_vec_bytes(&self.associated)
            + small_vec_bytes(&self.owners)
            + small_vec_bytes(&self.disassociated)
            + self
                .owners
                .iter()
                .map(|owner| {
                    small_vec_bytes(&owner.css_animations)
                        + small_vec_bytes(&owner.transitions)
                        + size_of_val::<[Arc<EffectDescription>]>(&owner.published_effects)
                })
                .sum::<usize>()
    }
}

/// Publishes `animations` to `engine` as the CSS animations the owner at `slot` holds, in its order: the name of each and
/// the definition last applied to it, which the computation of the owner's animation definitions matches its
/// definitions against.
fn publish_css_defined_animations(
    engine: StyleEngineTarget<'_>,
    slot: AnimationOwnerSlot,
    animations: &[AnimationHandle],
) {
    engine
        .host
        .queue_change(ArenaChange::Engine(EngineWrite::CssDefinedAnimations {
            node: engine.node,
            slot: slot.0,
            animations: animations
                .iter()
                .map(|animation| CssDefinedAnimation::of(animation))
                .collect(),
        }));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::animations::animation::AnimationKind;
    use crate::animations::effect::EffectHandle;

    fn element_at(address: &mut u8) -> NonNull<c_void> {
        NonNull::from(address).cast()
    }

    fn animation_targeting(element: NonNull<c_void>) -> AnimationHandle {
        let effect = EffectHandle::new();
        effect.borrow_mut().set_target_element(Some(element));
        let animation = AnimationHandle::new(AnimationKind::Script);
        animation.borrow_mut().set_effect(Some(effect));
        animation
    }

    fn target_of(animation: &AnimationHandle) -> Option<NonNull<c_void>> {
        animation
            .borrow()
            .effect()
            .and_then(|effect| effect.borrow().target_element())
    }

    #[test]
    fn finalizing_an_element_clears_the_target_of_an_animation_it_let_go_of() {
        let mut element = 0;
        let element = element_at(&mut element);
        let animation = animation_targeting(element);
        let mut animations = ElementAnimations::default();
        animations.associate(animation.clone());
        animations.disassociate(&animation);
        animations.forget_element_as_target_of_disassociated(element);
        assert_eq!(target_of(&animation), None);
    }

    #[test]
    fn finalizing_an_element_keeps_the_target_an_animation_it_let_go_of_moved_to() {
        let (mut first, mut second) = (0, 0);
        let (first, second) = (element_at(&mut first), element_at(&mut second));
        let animation = animation_targeting(first);
        let mut animations = ElementAnimations::default();
        animations.associate(animation.clone());
        animations.disassociate(&animation);
        let effect = animation.borrow().effect().cloned().unwrap();
        effect.borrow_mut().set_target_element(Some(second));
        animations.forget_element_as_target_of_disassociated(first);
        assert_eq!(target_of(&animation), Some(second));
    }

    #[test]
    fn an_animation_associated_again_is_no_longer_held_as_let_go_of() {
        let mut element = 0;
        let element = element_at(&mut element);
        let animation = animation_targeting(element);
        let mut animations = ElementAnimations::default();
        animations.associate(animation.clone());
        animations.disassociate(&animation);
        animations.associate(animation.clone());
        assert!(animations.disassociated.is_empty());
    }
}
