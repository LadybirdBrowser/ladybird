/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WebContent's end of the render clock: a thread of its own that asks the Compositor for display ticks for the
//! compositor contexts that are armed, and hands each tick it is delivered to the clock lanes the context was armed
//! for, without passing through the main thread. There is one per process; a Compositor reconnect swaps its channel,
//! not its thread. The Compositor's end is Services/Compositor/RenderClockConnection.
//!
//! Other threads reach the clock thread only by handing it jobs, in order. It owns the channel and the armed contexts,
//! and is the only one that hands ticks on.

use super::hover::{PendingPointer, PointerAnswer};
use super::{ClockTicks, FfiScrollOffset};
use crate::ipc_types::{Web_CompositorScrollOffset, render_clock_client, render_clock_server};
use crate::render_state::DocumentHost;
use libcompositing_rust::display_list::commands::{CompositorContextId, CompositorScrollNodeKind};
use libgfx_rust::FloatPoint;
use libipc_rust::EndpointMessage;
use smallvec::SmallVec;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, OnceLock};
use std::thread::ThreadId;

#[cfg(target_vendor = "apple")]
use libipc_rust::transport::TransportMachPort as Transport;
#[cfg(all(unix, not(target_vendor = "apple")))]
use libipc_rust::transport_socket::TransportSocket as Transport;
#[cfg(windows)]
use libipc_rust::transport_windows::TransportWindows as Transport;

/// The clock lanes of a document, which the render clock hands display ticks and pointer moves.
pub(crate) trait ClockLane: Clone + Send + 'static {
    /// Hands the lanes a tick, with where the Compositor had scrolled to then, and answers whether they want the next.
    fn tick(&self, frame_time_nanoseconds: i64, scroll_offsets: &[FfiScrollOffset]) -> bool;

    /// Hands the lanes where the pointer went, and answers what they want next.
    fn pointer_moved(&self, pointer: PendingPointer) -> PointerAnswer;

    /// Whether these are the lanes of the same document as `other`.
    fn is_same_lane_as(&self, other: &Self) -> bool;
}

impl ClockLane for Arc<ClockTicks> {
    fn tick(&self, frame_time_nanoseconds: i64, scroll_offsets: &[FfiScrollOffset]) -> bool {
        ClockTicks::tick(self, frame_time_nanoseconds, scroll_offsets)
    }

    fn pointer_moved(&self, pointer: PendingPointer) -> PointerAnswer {
        ClockTicks::pointer_moved(self, pointer)
    }

    fn is_same_lane_as(&self, other: &Self) -> bool {
        Arc::ptr_eq(self, other)
    }
}

enum Job<Lane> {
    Attach {
        generation: u64,
        transport: Arc<Transport>,
    },
    ArmLane {
        context: CompositorContextId,
        maximum_frames_per_second: f64,
        lane: Lane,
        tick_now_at: Option<i64>,
    },
    ForgetContext(CompositorContextId),
    DisarmAll,
    PointerMove {
        context: CompositorContextId,
        pointer: PendingPointer,
    },
    Incoming {
        generation: u64,
        message: render_clock_client::Message,
    },
    Lost {
        generation: u64,
    },
    #[cfg(test)]
    Nothing,
}

/// A job, and who waits for it to have run.
struct Envelope<Lane> {
    job: Job<Lane>,
    done: Option<mpsc::Sender<()>>,
}

pub(crate) struct RenderClock<Lane: ClockLane> {
    jobs: mpsc::Sender<Envelope<Lane>>,
    clock_thread: ThreadId,
    manual_for_testing: Arc<AtomicBool>,
    next_generation: AtomicU64,
}

impl<Lane: ClockLane> RenderClock<Lane> {
    /// Starts the clock thread, which runs until every handle to it is gone.
    pub(crate) fn new() -> std::io::Result<Self> {
        let (jobs, receiver) = mpsc::channel();
        let manual_for_testing = Arc::new(AtomicBool::new(false));
        let thread_manual_for_testing = Arc::clone(&manual_for_testing);
        let clock_thread = std::thread::Builder::new()
            .name("RenderClock".to_string())
            .spawn(move || ClockState::<Lane>::new(thread_manual_for_testing).run(&receiver))?;
        Ok(Self {
            jobs,
            clock_thread: clock_thread.thread().id(),
            manual_for_testing,
            next_generation: AtomicU64::new(1),
        })
    }

    fn post(&self, job: Job<Lane>) {
        // NB: The clock thread runs until every sender is gone, and this holds one.
        let _ = self.jobs.send(Envelope { job, done: None });
    }

    /// Runs `job` on the clock thread, and waits for it, unless this is the clock thread, which only queues it.
    fn run_and_wait(&self, job: Job<Lane>) {
        if std::thread::current().id() == self.clock_thread {
            self.post(job);
            return;
        }
        let (done, ran) = mpsc::channel();
        if self.jobs.send(Envelope { job, done: Some(done) }).is_ok() {
            let _ = ran.recv();
        }
    }

    /// Takes up a channel to the Compositor in place of the previous one, with nothing armed.
    pub(crate) fn attach(&self, transport: Transport) {
        let generation = self.next_generation.fetch_add(1, Ordering::Relaxed);
        let transport = Arc::new(transport);
        // NB: The channel is the clock thread's before anything it receives reaches that thread.
        self.post(Job::Attach {
            generation,
            transport: Arc::clone(&transport),
        });
        let jobs = self.jobs.clone();
        let spawned = std::thread::Builder::new()
            .name("RenderClock IPC".to_string())
            .spawn(move || receive_messages(&jobs, &transport, generation));
        if let Err(error) = spawned {
            eprintln!("RenderClock: Could not start receiving from the Compositor: {error}");
            self.post(Job::Lost { generation });
        }
    }

    /// Hands the pointer moves over the context to `lane` from now on, until another document's lanes take their place
    /// or the channel is lost, and arms the context's ticks for them where they ask for them; with `tick_now_at`, ticks
    /// them at once at that frame time, where no display tick drives them already. A context is armed until it
    /// declines a tick or the channel is lost; a tick that arrives for a context that is not armed is dropped.
    pub(crate) fn arm_lane(
        &self,
        context: CompositorContextId,
        maximum_frames_per_second: f64,
        lane: Lane,
        tick_now_at: Option<i64>,
    ) {
        // NB: The Compositor drops the connection of a process that asks for ticks at such a rate.
        assert!(maximum_frames_per_second.is_finite() && maximum_frames_per_second > 0.0);
        let job = Job::ArmLane {
            context,
            maximum_frames_per_second,
            lane,
            tick_now_at,
        };
        // A test hands the lanes pointer moves by hand, which find the context armed once the rendering update is over.
        if self.is_manual_for_testing() {
            self.run_and_wait(job);
        } else {
            self.post(job);
        }
    }

    /// Disarms the context, which is destroyed, and lets go of the lanes its pointer moves went to.
    pub(crate) fn forget_context(&self, context: CompositorContextId) {
        self.post(Job::ForgetContext(context));
    }

    /// Disarms every context, whose ticks a test injects itself from now on: no display tick reaches them after this
    /// returns.
    pub(crate) fn disarm_all_for_testing(&self) {
        self.run_and_wait(Job::DisarmAll);
    }

    /// Hands the lanes that hear of the pointer moves over the context a move, as the Compositor hands them those over a
    /// page's context. Asynchronous, but for a test that ticks by hand, whose lanes hear of it before this returns.
    pub(crate) fn hand_pointer_move(&self, context: CompositorContextId, pointer: PendingPointer) {
        let job = Job::PointerMove { context, pointer };
        if self.is_manual_for_testing() {
            self.run_and_wait(job);
        } else {
            self.post(job);
        }
    }

    /// Has a test tick the lanes by hand, whose pointer moves then arm no context.
    pub(crate) fn set_manual_for_testing(&self, manual: bool) {
        self.manual_for_testing.store(manual, Ordering::Relaxed);
    }

    pub(crate) fn is_manual_for_testing(&self) -> bool {
        self.manual_for_testing.load(Ordering::Relaxed)
    }

    /// Waits for the clock thread to have run every job handed to it before.
    #[cfg(test)]
    fn synchronize(&self) {
        self.run_and_wait(Job::Nothing);
    }
}

/// Hands the clock thread what the Compositor sends over the channel of `generation`, until the Compositor's end goes
/// away or sends something this end does not understand.
fn receive_messages<Lane>(jobs: &mpsc::Sender<Envelope<Lane>>, transport: &Transport, generation: u64) {
    while let Some(received) = transport.receive() {
        let (bytes, attachments) = received.into_parts();
        match render_clock_client::Message::decode(bytes.as_ref(), attachments) {
            Ok(message) => {
                let job = Job::Incoming { generation, message };
                if jobs.send(Envelope { job, done: None }).is_err() {
                    return;
                }
            }
            Err(error) => {
                eprintln!("RenderClock: Refusing a message from the Compositor: {error:?}");
                transport.shutdown();
                break;
            }
        }
    }
    let _ = jobs.send(Envelope {
        job: Job::Lost { generation },
        done: None,
    });
}

struct Channel {
    generation: u64,
    transport: Arc<Transport>,
}

struct Armed<Lane> {
    maximum_frames_per_second: f64,
    lane: Lane,
}

/// What the clock thread owns.
struct ClockState<Lane> {
    channel: Option<Channel>,
    armed_contexts: HashMap<CompositorContextId, Armed<Lane>>,
    pointer_lanes: HashMap<CompositorContextId, Armed<Lane>>,
    manual_for_testing: Arc<AtomicBool>,
}

impl<Lane: ClockLane> ClockState<Lane> {
    fn new(manual_for_testing: Arc<AtomicBool>) -> Self {
        Self {
            channel: None,
            armed_contexts: HashMap::new(),
            pointer_lanes: HashMap::new(),
            manual_for_testing,
        }
    }

    fn run(mut self, jobs: &mpsc::Receiver<Envelope<Lane>>) {
        while let Ok(Envelope { job, done }) = jobs.recv() {
            self.handle(job);
            if let Some(done) = done {
                let _ = done.send(());
            }
        }
        self.drop_channel();
    }

    fn handle(&mut self, job: Job<Lane>) {
        match job {
            Job::Attach { generation, transport } => {
                // A new channel starts with nothing armed: the contexts armed on the previous one were armed for a
                // Compositor that is gone.
                self.drop_channel();
                self.channel = Some(Channel { generation, transport });
            }
            Job::ArmLane {
                context,
                maximum_frames_per_second,
                lane,
                tick_now_at,
            } => self.arm_lane(context, maximum_frames_per_second, lane, tick_now_at),
            Job::ForgetContext(context) => {
                self.armed_contexts.remove(&context);
                self.pointer_lanes.remove(&context);
            }
            Job::DisarmAll => self.armed_contexts.clear(),
            Job::PointerMove { context, pointer } => self.did_receive_pointer_move(context, pointer),
            Job::Incoming { generation, message } => {
                // A message from a channel that has been replaced since.
                if !self.is_current_channel(generation) {
                    return;
                }
                self.did_receive_message(message);
            }
            Job::Lost { generation } => {
                if self.is_current_channel(generation) {
                    self.drop_channel();
                }
            }
            #[cfg(test)]
            Job::Nothing => {}
        }
    }

    fn is_current_channel(&self, generation: u64) -> bool {
        self.channel
            .as_ref()
            .is_some_and(|channel| channel.generation == generation)
    }

    fn drop_channel(&mut self) {
        self.armed_contexts.clear();
        self.pointer_lanes.clear();
        if let Some(channel) = self.channel.take() {
            channel.transport.shutdown();
        }
    }

    fn arm_lane(
        &mut self,
        context: CompositorContextId,
        maximum_frames_per_second: f64,
        lane: Lane,
        tick_now_at: Option<i64>,
    ) {
        // Another document of the context hears of the pointer no more, and a tick armed for it would hand its ticks to
        // this one's.
        if let Some(pointer_lane) = self.pointer_lanes.get(&context)
            && !pointer_lane.lane.is_same_lane_as(&lane)
        {
            self.armed_contexts.remove(&context);
        }
        // The display ticks of an armed context drive its ticks at their rate, however many tasks begin meanwhile.
        if let Some(tick_now_at) = tick_now_at
            && !self.armed_contexts.contains_key(&context)
        {
            let _ = lane.tick(tick_now_at, &[]);
            self.armed_contexts.insert(
                context,
                Armed {
                    maximum_frames_per_second,
                    lane: lane.clone(),
                },
            );
            self.request_clock_tick(context, maximum_frames_per_second);
        }
        self.pointer_lanes.insert(
            context,
            Armed {
                maximum_frames_per_second,
                lane,
            },
        );
    }

    fn did_receive_message(&mut self, message: render_clock_client::Message) {
        use render_clock_client::Message;
        match message {
            Message::ClockTick(tick) => {
                self.did_receive_clock_tick(tick.context_id, tick.frame_time_nanoseconds, &tick.scroll_offsets);
            }
            Message::PointerMoved(moved) => self.did_receive_pointer_move(
                moved.context_id,
                PendingPointer {
                    position: Some(FloatPoint {
                        x: moved.position.x as f32,
                        y: moved.position.y as f32,
                    }),
                    buttons: moved.buttons,
                    scrolled_since_frame: moved.scrolled_since_frame,
                    input_event_id: moved.input_event_id,
                },
            ),
            Message::PointerLeft(left) => self.did_receive_pointer_move(
                left.context_id,
                PendingPointer {
                    position: None,
                    buttons: 0,
                    scrolled_since_frame: false,
                    input_event_id: left.input_event_id,
                },
            ),
        }
    }

    fn did_receive_pointer_move(&mut self, context: CompositorContextId, pointer: PendingPointer) {
        let Some(pointer_lane) = self.pointer_lanes.get(&context) else {
            return;
        };
        match pointer_lane.lane.pointer_moved(pointer) {
            PointerAnswer::Moves => return,
            PointerAnswer::Ticks => {}
        }
        // A test ticks the lanes by hand.
        if self.manual_for_testing.load(Ordering::Relaxed) {
            return;
        }
        // A context armed for ticks already asked for the next one.
        if self.armed_contexts.contains_key(&context) {
            return;
        }
        let armed = Armed {
            maximum_frames_per_second: pointer_lane.maximum_frames_per_second,
            lane: pointer_lane.lane.clone(),
        };
        let maximum_frames_per_second = armed.maximum_frames_per_second;
        self.armed_contexts.insert(context, armed);
        self.request_clock_tick(context, maximum_frames_per_second);
    }

    fn did_receive_clock_tick(
        &mut self,
        context: CompositorContextId,
        frame_time_nanoseconds: i64,
        scroll_offsets: &[Web_CompositorScrollOffset],
    ) {
        // A tick for a context disarmed after its request went out, or armed on an earlier channel.
        let Some(armed) = self.armed_contexts.get(&context) else {
            return;
        };
        // NB: The lanes scroll the nodes of elements and viewports only.
        let scroll_offsets: SmallVec<[FfiScrollOffset; 8]> = scroll_offsets
            .iter()
            .filter(|offset| offset.scroll_node.kind != CompositorScrollNodeKind::PseudoElement)
            .map(|offset| FfiScrollOffset {
                scroller: offset.scroll_node.node_id.0,
                x: offset.offset.x.to_double(),
                y: offset.offset.y.to_double(),
            })
            .collect();
        // Handing a tick on only queues it, so the next request goes out right after: however long this tick takes, the
        // next one is already on its way, and the Compositor's pacing is the only limit on the rate.
        if !armed.lane.tick(frame_time_nanoseconds, &scroll_offsets) {
            self.armed_contexts.remove(&context);
            return;
        }
        let maximum_frames_per_second = armed.maximum_frames_per_second;
        self.request_clock_tick(context, maximum_frames_per_second);
    }

    fn request_clock_tick(&self, context: CompositorContextId, maximum_frames_per_second: f64) {
        let Some(channel) = &self.channel else {
            return;
        };
        let request = render_clock_server::messages::RequestClockTick {
            context_id: context,
            maximum_frames_per_second,
        };
        match request.encode_message() {
            Ok((bytes, attachments)) => {
                // NB: A channel whose Compositor is gone reports so on its own, through receive_messages().
                let _ = channel.transport.post_message(bytes, attachments);
            }
            Err(error) => eprintln!("RenderClock: Could not encode a clock tick request: {error:?}"),
        }
    }
}

/// The process's render clock, made the first time it is asked for.
fn the() -> &'static RenderClock<Arc<ClockTicks>> {
    static RENDER_CLOCK: OnceLock<RenderClock<Arc<ClockTicks>>> = OnceLock::new();
    RENDER_CLOCK.get_or_init(|| RenderClock::new().expect("the render clock thread starts"))
}

/// Hands the render clock a pointer move over the compositor context of a same-process iframe's document, as the
/// Compositor hands it those over the page's. Any thread may call it, and it ignores a context it has no lane for.
pub(super) fn hand_pointer_move(context: u64, pointer: PendingPointer) {
    the().hand_pointer_move(CompositorContextId(context), pointer);
}

fn attach(transport: std::io::Result<Transport>) -> bool {
    match transport {
        Ok(transport) => {
            the().attach(transport);
            true
        }
        Err(error) => {
            eprintln!("RenderClock: Could not take up the channel to the Compositor: {error}");
            false
        }
    }
}

/// Has the render clock take up the channel to the Compositor that `descriptor` is this process's end of, in place of
/// the previous one: a connected socket, or on Windows the handle of an overlapped pipe, as `IPC::File` holds it.
/// Answers whether it could.
///
/// # Safety
///
/// `descriptor` must be open, and the caller gives it up: nothing else owns it.
#[cfg(not(target_vendor = "apple"))]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_clock_attach_descriptor(descriptor: isize) -> bool {
    #[cfg(unix)]
    let transport = {
        use std::os::fd::FromRawFd;
        // SAFETY: Guaranteed by the caller.
        let socket = unsafe { std::os::unix::net::UnixStream::from_raw_fd(descriptor as std::os::fd::RawFd) };
        Transport::new(socket)
    };
    #[cfg(windows)]
    let transport = {
        use std::os::windows::io::FromRawHandle;
        // SAFETY: Guaranteed by the caller.
        let pipe = unsafe {
            std::os::windows::io::OwnedHandle::from_raw_handle(descriptor as std::os::windows::io::RawHandle)
        };
        Transport::new(pipe)
    };
    attach(transport)
}

/// Has the render clock take up the channel to the Compositor whose rights in this task are `receive_right` and
/// `send_right`, in place of the previous one. Answers whether it could.
///
/// # Safety
///
/// Both must be rights this task holds, and the caller gives them up: nothing else owns them.
#[cfg(target_vendor = "apple")]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_clock_attach_mach_rights(receive_right: u32, send_right: u32) -> bool {
    use libipc_rust::mach::{MachPort, PortRight};
    let receive_right = MachPort::adopt(receive_right, PortRight::Receive);
    let send_right = MachPort::adopt(send_right, PortRight::Send);
    attach(Transport::new(receive_right, send_right))
}

/// Has the render clock hand the clock lanes of `host`'s document the pointer moves over the compositor context
/// `context`, and the display ticks they ask for at up to `maximum_frames_per_second`; with `tick_now`, ticks them at
/// once at `tick_now_at`, where no display tick drives them.
#[unsafe(no_mangle)]
pub extern "C" fn render_clock_arm_lane(
    context: u64,
    maximum_frames_per_second: f64,
    host: &DocumentHost,
    tick_now: bool,
    tick_now_at: i64,
) {
    the().arm_lane(
        CompositorContextId(context),
        maximum_frames_per_second,
        Arc::clone(host.clock_ticks()),
        tick_now.then_some(tick_now_at),
    );
}

/// Has the render clock forget the compositor context `context`, which is destroyed.
#[unsafe(no_mangle)]
pub extern "C" fn render_clock_forget_context(context: u64) {
    the().forget_context(CompositorContextId(context));
}

/// Disarms every compositor context, whose ticks a test injects itself from now on.
#[unsafe(no_mangle)]
pub extern "C" fn render_clock_disarm_all_for_testing() {
    the().disarm_all_for_testing();
}

/// Has a test tick the clock lanes by hand, or not.
#[unsafe(no_mangle)]
pub extern "C" fn render_clock_set_manual_for_testing(manual: bool) {
    the().set_manual_for_testing(manual);
}

/// Whether a test ticks the clock lanes by hand.
#[unsafe(no_mangle)]
pub extern "C" fn render_clock_is_manual_for_testing() -> bool {
    the().is_manual_for_testing()
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use crate::ipc_types::{Web_AsyncScrollNodeStableID, Web_CSSPixelPoint, Web_DevicePixelPoint};
    use libcompositing_rust::css_pixels::CssPixels;
    use libcompositing_rust::display_list::commands::UniqueNodeId;
    use std::sync::Mutex;
    use std::time::Duration;

    #[cfg(target_vendor = "apple")]
    fn paired() -> (Transport, Transport) {
        let (ours, theirs) = Transport::create_paired().unwrap();
        (ours, Transport::from_handle(theirs).unwrap())
    }

    #[cfg(not(target_vendor = "apple"))]
    fn paired() -> (Transport, Transport) {
        let (ours, theirs) = Transport::create_paired().unwrap();
        (ours, Transport::new(theirs).unwrap())
    }

    #[derive(Default)]
    struct Record {
        ticks: Vec<(i64, Vec<FfiScrollOffset>)>,
        pointers: Vec<PendingPointer>,
        wants_ticks: usize,
        pointer_answer_ticks: bool,
    }

    /// The lanes of a document, which record what they are handed, and want as many ticks as `wants_ticks` says.
    #[derive(Clone, Default)]
    struct FakeLane(Arc<Mutex<Record>>);

    impl FakeLane {
        fn wanting_ticks(count: usize) -> Self {
            let lane = Self::default();
            lane.record().wants_ticks = count;
            lane
        }

        fn record(&self) -> std::sync::MutexGuard<'_, Record> {
            self.0.lock().unwrap_or_else(|error| error.into_inner())
        }
    }

    impl ClockLane for FakeLane {
        fn tick(&self, frame_time_nanoseconds: i64, scroll_offsets: &[FfiScrollOffset]) -> bool {
            let mut record = self.record();
            record.ticks.push((frame_time_nanoseconds, scroll_offsets.to_vec()));
            if record.wants_ticks == 0 {
                return false;
            }
            record.wants_ticks -= 1;
            true
        }

        fn pointer_moved(&self, pointer: PendingPointer) -> PointerAnswer {
            let mut record = self.record();
            record.pointers.push(pointer);
            if record.pointer_answer_ticks {
                PointerAnswer::Ticks
            } else {
                PointerAnswer::Moves
            }
        }

        fn is_same_lane_as(&self, other: &Self) -> bool {
            Arc::ptr_eq(&self.0, &other.0)
        }
    }

    /// A context of the tests' own, whose ticks tell when the clock has handled what came before them.
    const SENTINEL_CONTEXT: CompositorContextId = CompositorContextId(u64::MAX);

    /// The Compositor's end of a channel.
    struct Compositor(Transport);

    impl Compositor {
        fn attach_to(clock: &RenderClock<FakeLane>) -> Self {
            let (ours, theirs) = paired();
            clock.attach(theirs);
            clock.synchronize();
            Self(ours)
        }

        fn take_request(&self) -> (CompositorContextId, f64) {
            let received = self.0.receive().expect("a request arrives");
            let (bytes, attachments) = received.into_parts();
            let Ok(render_clock_server::Message::RequestClockTick(request)) =
                render_clock_server::Message::decode(bytes.as_ref(), attachments)
            else {
                panic!("the render clock sends only tick requests");
            };
            (request.context_id, request.maximum_frames_per_second)
        }

        /// Waits until the clock has handled every job handed to it and every message this sent, and answers the
        /// requests it sent meanwhile. A tick of the sentinel context goes after them all, and its request comes back
        /// once the clock has handled it.
        fn requests_after_everything_is_handled(
            &self,
            clock: &RenderClock<FakeLane>,
        ) -> Vec<(CompositorContextId, f64)> {
            let mut requests = Vec::new();
            let take_requests_until_the_sentinel = |requests: &mut Vec<_>| {
                loop {
                    let request = self.take_request();
                    if request.0 == SENTINEL_CONTEXT {
                        return;
                    }
                    requests.push(request);
                }
            };
            clock.arm_lane(SENTINEL_CONTEXT, 1.0, FakeLane::wanting_ticks(usize::MAX), Some(0));
            take_requests_until_the_sentinel(&mut requests);
            self.tick(SENTINEL_CONTEXT, 0, Vec::new());
            take_requests_until_the_sentinel(&mut requests);
            clock.forget_context(SENTINEL_CONTEXT);
            requests
        }

        fn send(&self, message: impl EndpointMessage) {
            let (bytes, attachments) = message.encode_message().unwrap();
            self.0.post_message(bytes, attachments).unwrap();
        }

        fn tick(
            &self,
            context: CompositorContextId,
            frame_time_nanoseconds: i64,
            scroll_offsets: Vec<Web_CompositorScrollOffset>,
        ) {
            self.send(render_clock_client::messages::ClockTick {
                context_id: context,
                frame_time_nanoseconds,
                frame_interval_milliseconds: 16.0,
                scroll_offsets,
            });
        }
    }

    fn scroll_offset(node: i64, kind: CompositorScrollNodeKind, x: i32) -> Web_CompositorScrollOffset {
        Web_CompositorScrollOffset {
            scroll_node: Web_AsyncScrollNodeStableID {
                node_id: UniqueNodeId(node),
                kind,
                pseudo_element_type: 0,
            },
            offset: Web_CSSPixelPoint {
                x: CssPixels::from_raw(x * 64),
                y: CssPixels::from_raw(0),
            },
        }
    }

    fn pointer_left(input_event_id: u64) -> PendingPointer {
        PendingPointer {
            position: None,
            buttons: 0,
            scrolled_since_frame: false,
            input_event_id,
        }
    }

    const CONTEXT: CompositorContextId = CompositorContextId(4);

    #[test]
    fn arming_with_a_tick_now_ticks_and_asks_for_the_next() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let compositor = Compositor::attach_to(&clock);
        // The tick now, and the first display tick.
        let lane = FakeLane::wanting_ticks(2);

        clock.arm_lane(CONTEXT, 120.0, lane.clone(), Some(1000));
        // A display tick reaches the lane with the scroll offsets of elements, and the lane wants one more.
        compositor.tick(
            CONTEXT,
            2000,
            vec![
                scroll_offset(10, CompositorScrollNodeKind::Element, 5),
                scroll_offset(11, CompositorScrollNodeKind::PseudoElement, 6),
            ],
        );
        assert_eq!(
            compositor.requests_after_everything_is_handled(&clock),
            [(CONTEXT, 120.0), (CONTEXT, 120.0)]
        );
        {
            let record = lane.record();
            assert_eq!(record.ticks.len(), 2);
            assert_eq!(record.ticks[0].0, 1000);
            assert!(record.ticks[0].1.is_empty());
            assert_eq!(record.ticks[1].0, 2000);
            assert_eq!(record.ticks[1].1.len(), 1);
            assert_eq!(record.ticks[1].1[0].scroller, 10);
            assert_eq!(record.ticks[1].1[0].x, 5.0);
        }

        // A tick the lane declines disarms the context: no request follows, and later ticks are dropped.
        compositor.tick(CONTEXT, 3000, Vec::new());
        compositor.tick(CONTEXT, 4000, Vec::new());
        assert!(compositor.requests_after_everything_is_handled(&clock).is_empty());
        assert_eq!(lane.record().ticks.len(), 3);
    }

    #[test]
    fn arming_without_a_tick_now_waits_for_the_pointer() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let compositor = Compositor::attach_to(&clock);
        let lane = FakeLane::default();
        clock.arm_lane(CONTEXT, 60.0, lane.clone(), None);

        // A move the lanes only take arms nothing.
        compositor.send(render_clock_client::messages::PointerMoved {
            context_id: CONTEXT,
            position: Web_DevicePixelPoint { x: 3, y: 4 },
            buttons: 1,
            scrolled_since_frame: true,
            input_event_id: 9,
        });
        assert!(compositor.requests_after_everything_is_handled(&clock).is_empty());
        {
            let record = lane.record();
            assert_eq!(record.pointers.len(), 1);
            assert_eq!(record.pointers[0].position, Some(FloatPoint { x: 3.0, y: 4.0 }));
            assert_eq!(record.pointers[0].buttons, 1);
            assert!(record.pointers[0].scrolled_since_frame);
            assert_eq!(record.pointers[0].input_event_id, 9);
        }

        // A move the lanes want a tick for arms the context.
        lane.record().pointer_answer_ticks = true;
        compositor.send(render_clock_client::messages::PointerLeft {
            context_id: CONTEXT,
            input_event_id: 10,
        });
        assert_eq!(
            compositor.requests_after_everything_is_handled(&clock),
            [(CONTEXT, 60.0)]
        );
        assert_eq!(lane.record().pointers[1].position, None);
        assert_eq!(lane.record().pointers[1].input_event_id, 10);
    }

    #[test]
    fn manual_mode_hands_pointer_moves_on_at_once_and_arms_nothing() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let compositor = Compositor::attach_to(&clock);
        let lane = FakeLane::default();
        lane.record().pointer_answer_ticks = true;
        clock.set_manual_for_testing(true);
        clock.arm_lane(CONTEXT, 60.0, lane.clone(), None);
        clock.hand_pointer_move(CONTEXT, pointer_left(1));
        // NB: No waiting: in manual mode, the lanes hear of the move before the call returns.
        assert_eq!(lane.record().pointers.len(), 1);
        assert!(compositor.requests_after_everything_is_handled(&clock).is_empty());
    }

    #[test]
    fn another_documents_lanes_take_over_a_context() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let compositor = Compositor::attach_to(&clock);
        let first = FakeLane::wanting_ticks(10);
        clock.arm_lane(CONTEXT, 60.0, first.clone(), Some(1));

        // The next document's lanes disarm the context, so the tick in flight is dropped rather than handed to them.
        let second = FakeLane::default();
        clock.arm_lane(CONTEXT, 60.0, second.clone(), None);
        compositor.tick(CONTEXT, 2, Vec::new());
        assert_eq!(
            compositor.requests_after_everything_is_handled(&clock),
            [(CONTEXT, 60.0)]
        );
        assert_eq!(first.record().ticks.len(), 1);
        assert!(second.record().ticks.is_empty());
    }

    #[test]
    fn forgetting_a_context_drops_its_ticks() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let compositor = Compositor::attach_to(&clock);
        let lane = FakeLane::wanting_ticks(10);
        clock.arm_lane(CONTEXT, 60.0, lane.clone(), Some(1));
        clock.forget_context(CONTEXT);
        compositor.tick(CONTEXT, 2, Vec::new());
        assert_eq!(
            compositor.requests_after_everything_is_handled(&clock),
            [(CONTEXT, 60.0)]
        );
        assert_eq!(lane.record().ticks.len(), 1);
    }

    #[test]
    fn a_new_channel_starts_with_nothing_armed() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let old_compositor = Compositor::attach_to(&clock);
        let lane = FakeLane::wanting_ticks(10);
        clock.arm_lane(CONTEXT, 60.0, lane.clone(), Some(1));
        assert_eq!(
            old_compositor.requests_after_everything_is_handled(&clock),
            [(CONTEXT, 60.0)]
        );

        // The old channel is shut down, and the new one has nothing armed.
        let new_compositor = Compositor::attach_to(&clock);
        assert!(old_compositor.0.receive().is_none());
        new_compositor.tick(CONTEXT, 2, Vec::new());
        assert!(new_compositor.requests_after_everything_is_handled(&clock).is_empty());
        assert_eq!(lane.record().ticks.len(), 1);

        // Arming again works on the new channel.
        clock.arm_lane(CONTEXT, 30.0, lane.clone(), Some(3));
        assert_eq!(
            new_compositor.requests_after_everything_is_handled(&clock),
            [(CONTEXT, 30.0)]
        );
    }

    #[test]
    fn losing_the_channel_disarms_everything() {
        let clock = RenderClock::<FakeLane>::new().unwrap();
        let compositor = Compositor::attach_to(&clock);
        let lane = FakeLane::default();
        clock.arm_lane(CONTEXT, 60.0, lane.clone(), None);
        let move_pointer = || {
            clock.hand_pointer_move(CONTEXT, pointer_left(1));
            clock.synchronize();
            lane.record().pointers.len()
        };
        assert_eq!(move_pointer(), 1);

        // NB: The clock hears of the loss once its receive thread sees the end of the stream, a moment after this.
        drop(compositor);
        let mut moves_heard = 1;
        for _ in 0..1000 {
            let heard = move_pointer();
            if heard == moves_heard {
                break;
            }
            moves_heard = heard;
            std::thread::sleep(Duration::from_millis(2));
        }
        assert_eq!(move_pointer(), moves_heard);
    }
}
