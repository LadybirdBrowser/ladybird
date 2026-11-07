/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The channel between two MessagePorts in one process, which may be in the agents of different threads.
//!
//! As in WebKit's MessagePortChannel, the channel owns the queues, so that a message waits in the queue of its side no
//! matter who owns that side, and a port that is transferred leaves its messages where they are. Only plain data is ever
//! in it: nothing of a heap crosses, and the receiver makes its record from the plain data on its own thread. An end
//! holds a share of the channel, so either side can go first, and a wake only names a read hook that the event loop looks
//! up on the thread of the receiver, where it finds nothing if the port went away.
//!
//! A channel stops being one when an end leaves the process. Its peer then finds a transport handle on its side, and
//! changes over to that when it gets to it. This happens at most once: an end that goes away never comes back, and a
//! channel does not form again between ports that a transport connects.

use core::hash::{BuildHasher, Hasher};
use std::collections::{HashMap, VecDeque};
use std::hash::RandomState;
use std::sync::{Arc, LazyLock, Mutex, MutexGuard, PoisonError};

use crate::html::message_port::{MessagePortTransport, TransportHandle};
use crate::html::structured_serialize::transfer_record::PortMessage;

/// Tells the owner of an end that there is something to read. Any thread may call it.
pub(crate) type Wake = Arc<dyn Fn() + Send + Sync>;

#[derive(Default)]
struct Side {
    /// The messages that wait for this side.
    queue: VecDeque<PortMessage>,
    wake: Option<Wake>,
    is_wake_pending: bool,
    /// The end of this side is gone, closed or out of this process, so nothing reaches it through the channel anymore.
    is_gone: bool,
    /// What this side changes over to, once the other side has left the process.
    transport: Option<TransportHandle>,
}

impl Side {
    fn has_news(&self, peer: &Side) -> bool {
        !self.queue.is_empty() || self.transport.is_some() || peer.is_gone
    }

    /// The wake to call once the channel is unlocked, unless the owner of this side has one on its way.
    fn wake_to_call(&mut self) -> Option<Wake> {
        if self.is_wake_pending {
            return None;
        }
        let wake = self.wake.clone()?;
        self.is_wake_pending = true;
        Some(wake)
    }
}

type Sides = [Side; 2];

fn lock(sides: &Mutex<Sides>) -> MutexGuard<'_, Sides> {
    sides.lock().unwrap_or_else(PoisonError::into_inner)
}

/// What read() found.
pub(crate) enum PeerState {
    Connected,
    Gone,
    /// The peer has left the process, and this is how to reach it.
    Transport(TransportHandle),
}

/// One end of a channel, which is what a MessagePortTransport or a data holder owns. Dropping it closes the end, and
/// what the end has sent stays readable for the peer.
pub(crate) struct PortChannelEnd {
    sides: Arc<Mutex<Sides>>,
    index: usize,
}

impl PortChannelEnd {
    pub(crate) fn create_pair() -> (Self, Self) {
        let sides = Arc::new(Mutex::new(Sides::default()));
        (
            Self {
                sides: Arc::clone(&sides),
                index: 0,
            },
            Self { sides, index: 1 },
        )
    }

    /// An end that another process handed over: its queued messages, and how to reach the peer, if it is still there.
    pub(crate) fn from_elsewhere(messages: Vec<PortMessage>, transport: Option<TransportHandle>) -> Self {
        let (end, peer) = Self::create_pair();
        {
            let mut sides = lock(&end.sides);
            sides[0].queue = messages.into();
            sides[0].transport = transport;
        }
        drop(peer);
        end
    }

    fn peer_index(&self) -> usize {
        1 - self.index
    }

    /// Sets what tells the owner of this end that there is something to read, or nothing for an end on its way to another
    /// owner. Calls it if there is already something to read.
    pub(crate) fn set_wake(&self, wake: Option<Wake>) {
        let mut sides = lock(&self.sides);
        let has_news = sides[self.index].has_news(&sides[self.peer_index()]);
        let side = &mut sides[self.index];
        side.wake = wake;
        side.is_wake_pending = false;
        let wake = if has_news { side.wake_to_call() } else { None };
        drop(sides);
        if let Some(wake) = wake {
            wake();
        }
    }

    /// Whether a message sent now can arrive.
    pub(crate) fn is_open(&self) -> bool {
        let sides = lock(&self.sides);
        sides[self.index].transport.is_some() || !sides[self.peer_index()].is_gone
    }

    /// Adds the message to the queue of the peer. A message for a peer that is gone is dropped, and one that has to go
    /// over a transport comes back.
    pub(crate) fn send(&self, message: PortMessage) -> Result<(), PortMessage> {
        let mut sides = lock(&self.sides);
        if sides[self.index].transport.is_some() {
            return Err(message);
        }
        let peer = &mut sides[self.peer_index()];
        if peer.is_gone {
            drop(sides);
            drop(message);
            return Ok(());
        }
        peer.queue.push_back(message);
        let wake = peer.wake_to_call();
        drop(sides);
        if let Some(wake) = wake {
            wake();
        }
        Ok(())
    }

    /// Takes all the messages that wait for this end, and tells what became of the peer.
    pub(crate) fn read(&self) -> (VecDeque<PortMessage>, PeerState) {
        let mut sides = lock(&self.sides);
        let peer_is_gone = sides[self.peer_index()].is_gone;
        let side = &mut sides[self.index];
        side.is_wake_pending = false;
        let messages = core::mem::take(&mut side.queue);
        let state = match side.transport.take() {
            Some(transport) => PeerState::Transport(transport),
            None if peer_is_gone => PeerState::Gone,
            None => PeerState::Connected,
        };
        (messages, state)
    }

    /// Moves the channel over to a transport as this end goes to another process, which gets what this returns: the
    /// messages that wait for this end, and the transport handle of its side, if the peer is still there.
    pub(crate) fn leave_process(self) -> (Vec<PortMessage>, Option<TransportHandle>) {
        let needs_new_transport = {
            let sides = lock(&self.sides);
            sides[self.index].transport.is_none() && !sides[self.peer_index()].is_gone
        };
        let new_transport = needs_new_transport.then(MessagePortTransport::create_paired_handles);

        // NB: The queue and the change over are one step: the peer can send until it finds its transport, and what it
        //     sends must be in the queue that goes along, or on that transport.
        let mut sides = lock(&self.sides);
        let peer_is_gone = sides[self.peer_index()].is_gone;
        let side = &mut sides[self.index];
        let messages: Vec<_> = core::mem::take(&mut side.queue).into();
        // NB: If the peer left the process at the same time, the transport that it made for this end is the one.
        if let Some(transport) = side.transport.take() {
            return (messages, Some(transport));
        }
        if peer_is_gone {
            return (messages, None);
        }
        let (own_handle, peer_handle) = new_transport.expect("a peer that is still there got a new transport");
        self.hand_peer_transport(sides, peer_handle);
        (messages, Some(own_handle))
    }

    /// Gives the peer the handle to change over to, and ends this side.
    fn hand_peer_transport(&self, mut sides: MutexGuard<'_, Sides>, handle: TransportHandle) {
        sides[self.index].is_gone = true;
        let peer = &mut sides[self.peer_index()];
        peer.transport = Some(handle);
        let wake = peer.wake_to_call();
        drop(sides);
        if let Some(wake) = wake {
            wake();
        }
    }
}

impl Drop for PortChannelEnd {
    fn drop(&mut self) {
        let mut sides = lock(&self.sides);
        let peer_index = self.peer_index();
        let side = &mut sides[self.index];
        let was_gone = core::mem::replace(&mut side.is_gone, true);
        side.wake = None;
        let queue = core::mem::take(&mut side.queue);
        let transport = side.transport.take();
        let wake = if was_gone {
            None
        } else {
            sides[peer_index].wake_to_call()
        };
        drop(sides);

        // NB: A message can hold ends of other channels, which lock these.
        drop(queue);
        drop(transport);
        if let Some(wake) = wake {
            wake();
        }
    }
}

/// An end on its way through the browser process, to a thread that its data holder does not reach directly. What reaches
/// the thread is a token, which finds the end here if it arrives in this process.
struct ParkedEnd {
    end: PortChannelEnd,
    /// The handle for the peer of the end, if the data holder also carries a transport, for the case that it arrives
    /// in another process.
    peer_transport: Option<TransportHandle>,
}

fn parked_ends() -> MutexGuard<'static, HashMap<u64, ParkedEnd>> {
    static PARKED_ENDS: LazyLock<Mutex<HashMap<u64, ParkedEnd>>> = LazyLock::new(Mutex::default);
    PARKED_ENDS.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Parks the end, and returns its token. If the end may arrive in another process, this also returns what the data
/// holder has to carry for that case.
pub(crate) fn park_end(end: PortChannelEnd, may_leave_process: bool) -> (u64, Option<TransportHandle>) {
    let (carried_transport, peer_transport) = if may_leave_process {
        let (carried_transport, peer_transport) = MessagePortTransport::create_paired_handles();
        (Some(carried_transport), Some(peer_transport))
    } else {
        (None, None)
    };
    let mut parked_ends = parked_ends();
    // NB: The token can arrive in another process, where it must not stand for an end of that process.
    let mut token = RandomState::new().build_hasher().finish();
    while token == 0 || parked_ends.contains_key(&token) {
        token = token.wrapping_add(1);
    }
    parked_ends.insert(token, ParkedEnd { end, peer_transport });
    (token, carried_transport)
}

/// The end that the token stands for, if it is still parked in this process.
pub(crate) fn claim_parked_end(token: u64) -> Option<PortChannelEnd> {
    let parked_end = parked_ends().remove(&token)?;
    Some(parked_end.end)
}

/// For an end that nobody in this process is going to claim: the peer goes on over the transport that the data holder
/// carries, if it has one, and otherwise finds its peer gone. An end that was claimed is left alone.
pub(crate) fn give_up_on_parked_end(token: u64) {
    let Some(ParkedEnd { end, peer_transport }) = parked_ends().remove(&token) else {
        return;
    };
    if let Some(peer_transport) = peer_transport {
        let sides = lock(&end.sides);
        end.hand_peer_transport(sides, peer_transport);
    }
}
