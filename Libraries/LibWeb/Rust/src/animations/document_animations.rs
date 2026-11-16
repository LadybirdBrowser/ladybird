/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document knows of the animations associated with it.

use super::animation::{AnimationCell, AnimationHandle, WeakAnimationSet};
use super::effect::{AnimationEffectCell, EffectHandle, WeakEffectSet};
use super::pending_animation_events::PendingAnimationEvents;
use super::provisional_transitions::ProvisionalTransitions;
use super::timeline::TimelineHandle;

/// The animations a document knows of, none of which it keeps alive, and the timelines associated with it, which it
/// does.
#[derive(Default)]
pub struct DocumentAnimations {
    /// The animations associated with the elements of the document, those of its shadow trees included.
    associated: WeakAnimationSet,
    /// The animations associated with the document while style and layout feedback is provisional, whose events
    /// stay provisional with them until the outermost style stabilization epoch commits.
    created_in_stabilization_epoch: WeakAnimationSet,
    /// The CSS animations of the elements of the document that follow a named timeline, which look it up again as
    /// the timelines the document declares change.
    following_named_timelines: WeakAnimationSet,
    /// The effects whose animated style the next animated style update samples.
    needing_animated_style_update: WeakEffectSet,
    /// The effects asked for an observation sample while an animated style update ran, which they are asked for as it
    /// finishes.
    needing_animated_style_update_after_current_update: WeakEffectSet,
    /// The effects dirtied beside the transaction of a style update that flew, which the next style update samples.
    needing_animated_style_update_beside_flown_style_update: WeakEffectSet,
    /// https://www.w3.org/TR/web-animations-1/#pending-animation-event-queue
    pub(crate) pending_events: PendingAnimationEvents,
    /// https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    /// In the order they were associated with the document.
    pub(crate) timelines: Vec<TimelineHandle>,
    /// https://www.w3.org/TR/web-animations-1/#document-default-document-timeline
    pub(crate) default_timeline: Option<TimelineHandle>,
    /// The transition actions the outermost style stabilization epoch decided, which it commits as it ends.
    pub(crate) provisional_transitions: ProvisionalTransitions,
}

/// One of the sets of animations a document knows of.
#[derive(Clone, Copy)]
pub enum DocumentAnimationSetKind {
    /// The animations associated with the elements of the document.
    Associated,
    /// The CSS animations of the elements of the document that follow a named timeline.
    FollowingNamedTimelines,
}

/// One of the sets of effects a document knows of.
#[expect(dead_code, reason = "C++ constructs the variants")]
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum DocumentEffectSetKind {
    /// The effects whose animated style the next animated style update samples.
    NeedingAnimatedStyleUpdate,
    /// The effects to ask for an observation sample as the current animated style update finishes.
    NeedingAnimatedStyleUpdateAfterCurrentUpdate,
    /// The effects dirtied beside the transaction of a style update that flew, which the next style update samples.
    SetAsideWhileFlownStyleUpdateDrains,
}

impl DocumentAnimations {
    pub(crate) fn effect_set_mut(&mut self, kind: DocumentEffectSetKind) -> &mut WeakEffectSet {
        match kind {
            DocumentEffectSetKind::NeedingAnimatedStyleUpdate => &mut self.needing_animated_style_update,
            DocumentEffectSetKind::NeedingAnimatedStyleUpdateAfterCurrentUpdate => {
                &mut self.needing_animated_style_update_after_current_update
            }
            DocumentEffectSetKind::SetAsideWhileFlownStyleUpdateDrains => {
                &mut self.needing_animated_style_update_beside_flown_style_update
            }
        }
    }

    /// Begins the outermost style stabilization epoch, during which the document has created no animations and queued
    /// no events yet.
    pub(crate) fn begin_style_stabilization_epoch(&mut self) {
        assert!(
            !self.pending_events.has_provisional_events(),
            "no events are provisional outside a style stabilization epoch"
        );
        self.created_in_stabilization_epoch.prune();
        assert!(
            self.created_in_stabilization_epoch.entry_count() == 0,
            "no animations are created in a style stabilization epoch before it begins"
        );
        assert!(
            self.provisional_transitions.is_empty(),
            "no transition actions are provisional outside a style stabilization epoch"
        );
    }

    /// Commits the events queued during the outermost style stabilization epoch to the pending animation event queue,
    /// as the epoch commits, and forgets which animations it created, answering how many events it committed.
    pub(crate) fn commit_provisional_events(&mut self) -> usize {
        let created_in_epoch = &self.created_in_stabilization_epoch;
        // An animation created and canceled inside this epoch was never part of an externally committed style. Its
        // cancellation is provisional too, so no observer may receive it.
        let committed = self.pending_events.commit_provisional_events(|animation| {
            created_in_epoch.contains(animation)
                && animation.borrow().play_state() == super::timing::AnimationPlayState::Idle
        });
        self.created_in_stabilization_epoch.clear();
        committed
    }

    /// Associates `animation` with the document, as one created during its style stabilization epoch where
    /// `in_style_stabilization_epoch`.
    pub(crate) fn associate(&mut self, animation: &AnimationHandle, in_style_stabilization_epoch: bool) {
        self.associated.insert(animation);
        if in_style_stabilization_epoch {
            self.created_in_stabilization_epoch.insert(animation);
        }
    }

    /// Disassociates `animation` from the document, whose associated effect `effect` was, which no animated style
    /// update of the document samples anymore.
    pub(crate) fn disassociate(&mut self, animation: &AnimationCell, effect: Option<&AnimationEffectCell>) {
        if let Some(effect) = effect {
            self.needing_animated_style_update.remove(effect);
            self.needing_animated_style_update_after_current_update.remove(effect);
            self.needing_animated_style_update_beside_flown_style_update
                .remove(effect);
        }
        self.associated.remove(animation);
    }

    /// The animations associated with the elements of the document that are alive.
    pub(crate) fn associated_animations(&mut self) -> Vec<AnimationHandle> {
        self.associated.alive()
    }

    /// The effects whose animated style the next animated style update samples.
    pub(crate) fn needing_animated_style_update(&mut self) -> &mut WeakEffectSet {
        &mut self.needing_animated_style_update
    }

    /// The effects asked for an observation sample while an animated style update ran.
    pub(crate) fn needing_animated_style_update_after_current_update(&mut self) -> &mut WeakEffectSet {
        &mut self.needing_animated_style_update_after_current_update
    }

    /// Sets aside the effects the next animated style update samples while the transaction of a style update that
    /// flew drains, or restores those set aside, where `restore`.
    pub(crate) fn set_aside_effects_needing_animated_style_update(&mut self, restore: bool) {
        let (from, to) = if restore {
            (
                &mut self.needing_animated_style_update_beside_flown_style_update,
                &mut self.needing_animated_style_update,
            )
        } else {
            (
                &mut self.needing_animated_style_update,
                &mut self.needing_animated_style_update_beside_flown_style_update,
            )
        };
        from.take_each(|effect: &EffectHandle| {
            to.insert(effect);
        });
    }

    pub(crate) fn set_mut(&mut self, kind: DocumentAnimationSetKind) -> &mut WeakAnimationSet {
        match kind {
            DocumentAnimationSetKind::Associated => &mut self.associated,
            DocumentAnimationSetKind::FollowingNamedTimelines => &mut self.following_named_timelines,
        }
    }
}
