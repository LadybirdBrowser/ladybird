/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's one way to wait for a document's render state, and the rights it spends to do so.
//!
//! The host waits only with a [`ScriptForcedRead`], which the host entries script APIs call mint for a current answer,
//! or in a [`BegunRead`], a read the host began, which a host entry that reaches the render state where the host is
//! takes from its caller: code that has not begun a read has nothing to reach the state with. What runs beside the host
//! is taken in without a wait only at a [`TaskBoundary`], which the entries the event loop calls between two tasks
//! mint.

use super::DocumentHost;
use std::marker::PhantomData;

/// The one wait of a script API call that needs a current answer: getComputedStyle, an element's geometry, hit
/// testing, innerText and the like. The host entry the API calls mints it, and a wait takes it by value, so it is
/// neither `Clone` nor `Copy`, and it stays on the document's thread.
pub(crate) struct ScriptForcedRead {
    not_send_or_sync: PhantomData<*const ()>,
}

/// The right of the host to wait for a recording in flight, for one of the reasons that have a [`LockstepReason`]
/// marker. It stays on the document's thread.
pub(crate) struct LockstepProof {
    not_send_or_sync: PhantomData<*const ()>,
}

/// A read of a document's render state that the host began and has not ended. A host entry that reaches the render
/// state where the host is takes it from its caller, as only a read takes a frame in flight in: the host's scope of a
/// read lends it to the entries it calls (see [`super::document_host::document_host_read_scope_view`]), and nothing
/// else makes one, so code that reaches the state without a begun read does not compile. It stays on the document's
/// thread.
pub struct BegunRead {
    not_send_or_sync: PhantomData<*const ()>,
}

impl BegunRead {
    /// The begun read a host keeps, which its scopes of a read lend.
    pub(super) fn of_host() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// What the reads of a layout node the host holds spend: the read that reached the node. A layout node is reached only
/// through an entry that takes a [`BegunRead`], as the ones that find the node an element or a row is bound to do, and
/// that entry took the frame in, which flies again only from a rendering update, after the task that reached the node.
/// Only the modules whose entries answer a node's reads of its row mint one, each with a [`HeldNodeEntry`] marker.
pub(crate) struct NodeRead {
    not_send_or_sync: PhantomData<*const ()>,
}

impl NodeRead {
    /// The read of a layout node the host holds, which the entry `_` marks reads.
    pub(crate) fn of_held_node(_: &impl HeldNodeEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// The top of the host's event loop, between two tasks, where the host takes in what has finished beside it and waits
/// for nothing. It stays on the host's thread.
pub(crate) struct TaskBoundary {
    not_send_or_sync: PhantomData<*const ()>,
}

/// The start of a task of the host's event loop, where a clock lease begins: only there, so a lease never begins inside
/// a task. It stays on the host's thread.
pub(crate) struct TaskStart {
    not_send_or_sync: PhantomData<*const ()>,
}

mod private {
    pub trait ScriptEntry {}
    pub trait LockstepReason {}
    pub trait EventLoopEntry {}
    pub trait RenderWait {}
    pub trait HeldNodeEntry {}
}

/// A marker that only a host entry script APIs call can construct, which mints a [`ScriptForcedRead`].
pub(crate) trait ScriptEntry: private::ScriptEntry {}

/// A marker that only a module whose entries read the row of a layout node the host holds can construct, which mints a
/// [`NodeRead`].
pub(crate) trait HeldNodeEntry: private::HeldNodeEntry {}

/// A marker that only the module waiting for its reason can construct, which mints a [`LockstepProof`] to join a
/// recording in flight.
pub(crate) trait LockstepReason: private::LockstepReason {}

/// A marker that only a host entry the event loop calls between two tasks can construct, which mints a
/// [`TaskBoundary`].
pub(crate) trait EventLoopEntry: private::EventLoopEntry {}

/// What a wait for a document's render state spends: a script's forced read, or the read the host began.
pub(crate) trait RenderWait: private::RenderWait {
    /// What takes a frame in flight in for the wait, which the wait spends only where the frame flies.
    fn into_forced_read(self) -> ForcedRead;

    /// Whether the wait may reach the render state of `host`'s document: a begun read reaches only the render state of
    /// the document whose host began it.
    fn reaches(&self, _host: &DocumentHost) -> bool {
        true
    }
}

impl private::RenderWait for ScriptForcedRead {}
impl RenderWait for ScriptForcedRead {
    fn into_forced_read(self) -> ForcedRead {
        ForcedRead::minted()
    }
}

impl private::RenderWait for &BegunRead {}
impl RenderWait for &BegunRead {
    fn into_forced_read(self) -> ForcedRead {
        ForcedRead::minted()
    }

    fn reaches(&self, host: &DocumentHost) -> bool {
        std::ptr::eq(*self, host.begun_read())
    }
}

impl private::RenderWait for ForcedRead {}
impl RenderWait for ForcedRead {
    fn into_forced_read(self) -> ForcedRead {
        self
    }
}

impl private::RenderWait for super::NoFrameInFlight {}
impl RenderWait for super::NoFrameInFlight {
    fn into_forced_read(self) -> ForcedRead {
        unreachable!("a frame flies only from a rendering update, which no reach without one runs")
    }
}

impl private::RenderWait for NodeRead {}
impl RenderWait for NodeRead {
    fn into_forced_read(self) -> ForcedRead {
        ForcedRead::minted()
    }
}

impl ScriptForcedRead {
    /// A forced read for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The forced read of the script API call that entered the host through the entry `_` marks.
    pub(crate) fn at_script_entry(_: &impl ScriptEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

impl LockstepProof {
    /// The right to wait for the reason `_` marks.
    pub(crate) fn for_reason(_: &impl LockstepReason) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

impl TaskBoundary {
    /// A task boundary for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The task boundary at which the event loop calls the entry `_` marks.
    pub(crate) fn at_event_loop_entry(_: &impl EventLoopEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

impl TaskStart {
    /// The start of the task at which the event loop calls the entry `_` marks.
    pub(crate) fn at_event_loop_entry(_: &impl EventLoopEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// What a wait for a frame in flight spends: the forced read of a script API call, a read the host began, or the host's
/// teardown of its document. Only a [`RenderWait`] makes one, but for the teardown.
pub(crate) struct ForcedRead {
    not_send_or_sync: PhantomData<*const ()>,
}

impl ForcedRead {
    fn minted() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The read of the host's teardown of its document.
    pub(super) fn of_teardown() -> Self {
        Self::minted()
    }
}

macro_rules! script_entry {
    ($marker:path) => {
        impl private::ScriptEntry for $marker {}
        impl ScriptEntry for $marker {}
    };
}

// The host entries script APIs call, which may each spend one forced read.
script_entry!(super::devtools::DevtoolsEntry);
script_entry!(crate::layout::script_entries::ScriptEntry);

macro_rules! lockstep_reason {
    ($marker:path) => {
        impl private::LockstepReason for $marker {}
        impl LockstepReason for $marker {}
    };
}

// The waits internal code makes for a recording, each minted only by the module its marker belongs to.
lockstep_reason!(crate::painting::recording_slot::RecordingNeedsItsRecorder);

/// Defines `HeldNode`, the marker of a module whose host entries read or write the row of a layout node the host holds,
/// and `node_read()`, the [`NodeRead`] they spend. The module's marker is listed below.
macro_rules! held_node_entries {
    () => {
        /// Marks the host entries of this module, which read or write the row of a layout node the host holds.
        pub(crate) struct HeldNode {
            _private: (),
        }

        /// The read of the layout node the host holds, which this module's entries spend.
        fn node_read() -> $crate::render_state::NodeRead {
            $crate::render_state::NodeRead::of_held_node(&HeldNode { _private: () })
        }
    };
}

pub(crate) use held_node_entries;

macro_rules! held_node_entry {
    ($marker:path) => {
        impl private::HeldNodeEntry for $marker {}
        impl HeldNodeEntry for $marker {}
    };
}

// The modules whose host entries read or write the row of a layout node the host holds.
held_node_entry!(crate::layout::shell_reads::HeldNode);
held_node_entry!(crate::layout::ArenaHeldNode);
held_node_entry!(crate::layout::PartialRelayoutHeldNode);
held_node_entry!(crate::layout::rendered_text::HeldNode);
held_node_entry!(crate::layout::text_queries::HeldNode);
held_node_entry!(crate::painting::ffi::HeldNode);
held_node_entry!(crate::painting::layout_tree_dump::HeldNode);
lockstep_reason!(super::clock::PresenterNeedsItsFrame);
lockstep_reason!(super::clock::AnimationChanged);
lockstep_reason!(super::clock::TestReadsPresentedFrame);

// The host entries the event loop calls between two tasks.
impl private::EventLoopEntry for crate::painting::ffi::TakesFinishedRecordingIn {}
impl EventLoopEntry for crate::painting::ffi::TakesFinishedRecordingIn {}
impl private::EventLoopEntry for crate::css::style::style_job::TakesFinishedStyleIn {}
impl EventLoopEntry for crate::css::style::style_job::TakesFinishedStyleIn {}
impl private::EventLoopEntry for super::clock::LeasesClockForTask {}
impl EventLoopEntry for super::clock::LeasesClockForTask {}

#[cfg(test)]
mod tests {
    use super::*;

    trait AmbiguousIfSend<A> {
        fn marker() {}
    }

    impl<T: ?Sized> AmbiguousIfSend<()> for T {}
    impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

    // These fail to compile, as the calls are ambiguous, if a right to wait ever becomes Send.
    #[test]
    fn a_script_forced_read_is_not_send() {
        <ScriptForcedRead as AmbiguousIfSend<_>>::marker();
    }

    #[test]
    fn a_lockstep_proof_is_not_send() {
        <LockstepProof as AmbiguousIfSend<_>>::marker();
    }

    #[test]
    fn a_task_boundary_is_not_send() {
        <TaskBoundary as AmbiguousIfSend<_>>::marker();
    }

    #[test]
    fn a_task_start_is_not_send() {
        <TaskStart as AmbiguousIfSend<_>>::marker();
    }
}
