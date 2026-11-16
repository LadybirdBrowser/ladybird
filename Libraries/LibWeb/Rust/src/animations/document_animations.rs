/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a document knows of the animations associated with it.

use super::animation::WeakAnimationSet;
use super::effect::WeakEffectSet;
use super::pending_animation_events::PendingAnimationEvents;

/// The animations a document knows of, none of which it keeps alive.
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
}

/// One of the sets of animations a document knows of.
#[expect(dead_code, reason = "C++ constructs the variants")]
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum DocumentAnimationSetKind {
    /// The animations associated with the elements of the document.
    Associated,
    /// The animations associated with the document during the current style stabilization epoch.
    CreatedInStabilizationEpoch,
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
    }

    /// Commits the events queued during the outermost style stabilization epoch to the pending animation event queue,
    /// as the epoch commits, and forgets which animations it created, answering how many events it committed.
    pub(crate) fn commit_provisional_events(&mut self) -> usize {
        let created_in_epoch = &self.created_in_stabilization_epoch;
        // An animation created and cancelled inside this epoch was never part of an externally committed style. Its
        // cancellation is provisional too, so no observer may receive it.
        let committed = self.pending_events.commit_provisional_events(|animation| {
            created_in_epoch.contains(animation)
                && animation.borrow().timing_state(None).play_state == super::timing::AnimationPlayState::Idle
        });
        self.created_in_stabilization_epoch.clear();
        committed
    }

    pub(crate) fn set_mut(&mut self, kind: DocumentAnimationSetKind) -> &mut WeakAnimationSet {
        match kind {
            DocumentAnimationSetKind::Associated => &mut self.associated,
            DocumentAnimationSetKind::CreatedInStabilizationEpoch => &mut self.created_in_stabilization_epoch,
            DocumentAnimationSetKind::FollowingNamedTimelines => &mut self.following_named_timelines,
        }
    }
}
