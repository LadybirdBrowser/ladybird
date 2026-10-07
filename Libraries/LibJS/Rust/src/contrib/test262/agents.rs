/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The processes behind $262.agent, as test262's INTERPRETING.md describes it.
//!
//! A LibGC heap belongs to one thread, so every agent a test starts is a child process: the tool runs again in agent
//! mode, with a VM of its own. Each agent has a Unix domain socket to the test, which sends it its source and then the
//! broadcasts, passing the shared memory of a SharedArrayBuffer with SCM_RIGHTS. The agent answers on the socket once
//! it is running and whenever a broadcast reaches it. Reports from all of a test's agents share one pipe, so the test
//! reads them in the order the agents wrote them. An agent exits when the test's end of its socket closes, so no agent
//! outlives the test that started it, even when the test is killed for running too long.

use core::ffi::c_int;
use std::cell::RefCell;
use std::collections::{HashMap, VecDeque};
use std::ffi::OsString;
use std::fs::File;
use std::io::{Read, Write};
use std::os::fd::{AsFd, AsRawFd, BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::os::unix::net::UnixStream;
use std::os::unix::process::ExitStatusExt;
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Condvar, Mutex, OnceLock, PoisonError};

const SOURCE_MESSAGE: u8 = 1;
const BROADCAST_MESSAGE: u8 = 2;
const AGENT_STARTED_ACKNOWLEDGEMENT: u8 = 3;
const BROADCAST_RECEIVED_ACKNOWLEDGEMENT: u8 = 4;

const UNDEFINED_BROADCAST_NUMBER: u8 = 0;
const NUMBER_BROADCAST_NUMBER: u8 = 1;
const BIGINT_BROADCAST_NUMBER: u8 = 2;

const REPORT_FRAME: u8 = 0;
const UNCAUGHT_EXCEPTION_FRAME: u8 = 1;
const PANIC_FRAME: u8 = 2;

/// _POSIX_PIPE_BUF: the most a write to a pipe may hold and still never interleave with what other agents write.
const ATOMIC_PIPE_WRITE_SIZE: usize = 512;
/// The agent's index, the frame kind, whether the frame ends its message, and the length of what follows.
const FRAME_HEADER_SIZE: usize = 4 + 1 + 1 + 2;
const FRAME_PAYLOAD_LIMIT: usize = ATOMIC_PIPE_WRITE_SIZE - FRAME_HEADER_SIZE;

#[cfg(target_vendor = "apple")]
const SEND_FLAGS: c_int = 0;
#[cfg(not(target_vendor = "apple"))]
const SEND_FLAGS: c_int = libc::MSG_NOSIGNAL;

/// The number that $262.agent.broadcast() passes along with its SharedArrayBuffer.
#[derive(Clone, Debug, PartialEq)]
pub enum BroadcastNumber {
    Undefined,
    Number(f64),
    /// The two's complement bytes of the BigInt, least significant first.
    BigInt(Vec<u8>),
}

/// A broadcast as an agent receives it: the SharedArrayBuffer's shared memory object and what names it, and the
/// number.
#[derive(Debug)]
pub struct Broadcast {
    pub shared_memory: OwnedFd,
    pub byte_length: usize,
    pub object_id: u64,
    pub number: BroadcastNumber,
}

/// Why an agent stopped before it sent the report that the test waits for.
#[derive(Clone, Debug, PartialEq)]
pub enum AgentFailure {
    UncaughtException { name: String, message: String },
    Panic(String),
    Exited(String),
}

/// Which half of $262.agent this process provides.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum AgentRole {
    /// Neither: the tool did not set up agents.
    Unavailable,
    /// The test's: start(), broadcast() and getReport().
    Test,
    /// An agent's: receiveBroadcast(), report() and leaving().
    Agent,
}

pub fn role() -> AgentRole {
    if TEST_CONNECTION.get().is_some() {
        return AgentRole::Agent;
    }
    if AGENT_LAUNCHER.with_borrow(Option::is_some) {
        return AgentRole::Test;
    }
    AgentRole::Unavailable
}

/// How to run this tool in agent mode.
struct AgentLauncher {
    program: PathBuf,
    arguments: Vec<OsString>,
}

thread_local! {
    static AGENT_LAUNCHER: RefCell<Option<AgentLauncher>> = const { RefCell::new(None) };
    static RUNNING_AGENTS: RefCell<RunningAgents> = RefCell::new(RunningAgents::default());
}

/// Lets tests start agents, each of which runs `program` with `arguments`, which must make it call connect_to_test().
pub fn enable_agents(program: PathBuf, arguments: Vec<OsString>) {
    AGENT_LAUNCHER.set(Some(AgentLauncher { program, arguments }));
}

fn send_all(socket: BorrowedFd<'_>, mut bytes: &[u8]) -> std::io::Result<()> {
    while !bytes.is_empty() {
        // SAFETY: The bytes are valid for their length.
        let sent = unsafe { libc::send(socket.as_raw_fd(), bytes.as_ptr().cast(), bytes.len(), SEND_FLAGS) };
        if sent < 0 {
            let error = std::io::Error::last_os_error();
            if error.kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return Err(error);
        }
        bytes = &bytes[sent.cast_unsigned()..];
    }
    Ok(())
}

/// Sends `bytes` with a duplicate of `descriptor` attached to the first of them.
fn send_with_descriptor(socket: BorrowedFd<'_>, bytes: &[u8], descriptor: BorrowedFd<'_>) -> std::io::Result<()> {
    let mut control_buffer = [0u64; 4];
    let mut io_vector = libc::iovec {
        iov_base: bytes.as_ptr().cast_mut().cast(),
        iov_len: bytes.len(),
    };
    // SAFETY: An all-zero msghdr is an empty message.
    let mut message: libc::msghdr = unsafe { core::mem::zeroed() };
    message.msg_iov = &raw mut io_vector;
    message.msg_iovlen = 1;
    message.msg_control = control_buffer.as_mut_ptr().cast();
    let descriptor_size = size_of::<c_int>() as u32;
    // SAFETY: CMSG_SPACE only computes a size.
    message.msg_controllen = unsafe { libc::CMSG_SPACE(descriptor_size) } as _;
    // SAFETY: The control buffer has room for one control message with one descriptor, which this fills in.
    unsafe {
        let header = libc::CMSG_FIRSTHDR(&raw const message);
        (*header).cmsg_level = libc::SOL_SOCKET;
        (*header).cmsg_type = libc::SCM_RIGHTS;
        (*header).cmsg_len = libc::CMSG_LEN(descriptor_size) as _;
        libc::CMSG_DATA(header)
            .cast::<c_int>()
            .write_unaligned(descriptor.as_raw_fd());
    }
    let sent = loop {
        // SAFETY: The message describes the bytes and the control buffer, which are all live.
        let sent = unsafe { libc::sendmsg(socket.as_raw_fd(), &raw const message, SEND_FLAGS) };
        if sent >= 0 {
            break sent.cast_unsigned();
        }
        let error = std::io::Error::last_os_error();
        if error.kind() != std::io::ErrorKind::Interrupted {
            return Err(error);
        }
    };
    send_all(socket, &bytes[sent..])
}

/// Keeps a write to a socket whose peer is gone from raising SIGPIPE, where send() has no flag for that.
#[cfg_attr(not(target_vendor = "apple"), allow(clippy::unnecessary_wraps))]
fn never_raise_sigpipe(socket: &UnixStream) -> std::io::Result<()> {
    #[cfg(target_vendor = "apple")]
    {
        let enabled: c_int = 1;
        // SAFETY: The option value is a c_int that outlives the call.
        let result = unsafe {
            libc::setsockopt(
                socket.as_raw_fd(),
                libc::SOL_SOCKET,
                libc::SO_NOSIGPIPE,
                (&raw const enabled).cast(),
                size_of::<c_int>() as libc::socklen_t,
            )
        };
        if result != 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    #[cfg(not(target_vendor = "apple"))]
    let _ = socket;
    Ok(())
}

/// What the test sends an agent.
#[derive(Debug)]
enum ControlMessage {
    Source { agent_index: u32, source: Vec<u8> },
    Broadcast(Broadcast),
}

fn encode_source_message(agent_index: u32, source: &[u8]) -> Vec<u8> {
    let mut message = vec![SOURCE_MESSAGE];
    message.extend_from_slice(&agent_index.to_le_bytes());
    message.extend_from_slice(
        &u32::try_from(source.len())
            .expect("an agent's source fits in u32")
            .to_le_bytes(),
    );
    message.extend_from_slice(source);
    message
}

/// A broadcast without its shared memory, which goes along as a descriptor.
fn encode_broadcast_message(byte_length: usize, object_id: u64, number: &BroadcastNumber) -> Vec<u8> {
    let mut message = vec![BROADCAST_MESSAGE];
    message.extend_from_slice(&(byte_length as u64).to_le_bytes());
    message.extend_from_slice(&object_id.to_le_bytes());
    match number {
        BroadcastNumber::Undefined => message.push(UNDEFINED_BROADCAST_NUMBER),
        BroadcastNumber::Number(number) => {
            message.push(NUMBER_BROADCAST_NUMBER);
            message.extend_from_slice(&number.to_bits().to_le_bytes());
        }
        BroadcastNumber::BigInt(bytes) => {
            message.push(BIGINT_BROADCAST_NUMBER);
            message.extend_from_slice(&u32::try_from(bytes.len()).expect("a BigInt fits in u32").to_le_bytes());
            message.extend_from_slice(bytes);
        }
    }
    message
}

/// Reads the messages of the test from an agent's socket, with the descriptors that came along.
#[derive(Debug)]
struct ControlReader {
    socket: OwnedFd,
    unread_bytes: VecDeque<u8>,
    received_descriptors: VecDeque<OwnedFd>,
}

impl ControlReader {
    fn new(socket: OwnedFd) -> Self {
        Self {
            socket,
            unread_bytes: VecDeque::new(),
            received_descriptors: VecDeque::new(),
        }
    }

    fn receive_more(&mut self) -> std::io::Result<()> {
        let mut buffer = [0u8; 4096];
        let mut control_buffer = [0u64; 32];
        let mut io_vector = libc::iovec {
            iov_base: buffer.as_mut_ptr().cast(),
            iov_len: buffer.len(),
        };
        // SAFETY: An all-zero msghdr is an empty message.
        let mut message: libc::msghdr = unsafe { core::mem::zeroed() };
        message.msg_iov = &raw mut io_vector;
        message.msg_iovlen = 1;
        message.msg_control = control_buffer.as_mut_ptr().cast();
        message.msg_controllen = size_of_val(&control_buffer) as _;
        let received = loop {
            // SAFETY: The message describes the buffers, which are live and writable.
            let received = unsafe { libc::recvmsg(self.socket.as_raw_fd(), &raw mut message, 0) };
            if received >= 0 {
                break received.cast_unsigned();
            }
            let error = std::io::Error::last_os_error();
            if error.kind() != std::io::ErrorKind::Interrupted {
                return Err(error);
            }
        };

        // SAFETY: recvmsg() filled in the control messages that the walk visits, and the kernel installed the
        //         descriptors of SCM_RIGHTS ones in this process, which now owns them.
        unsafe {
            let mut header = libc::CMSG_FIRSTHDR(&raw const message);
            while !header.is_null() {
                if (*header).cmsg_level == libc::SOL_SOCKET && (*header).cmsg_type == libc::SCM_RIGHTS {
                    let data = libc::CMSG_DATA(header);
                    #[allow(clippy::unnecessary_cast, reason = "cmsg_len is a socklen_t on some platforms")]
                    let data_length =
                        (*header).cmsg_len as usize - data.offset_from(header.cast::<u8>()).cast_unsigned();
                    for index in 0..data_length / size_of::<c_int>() {
                        let descriptor = data.cast::<c_int>().add(index).read_unaligned();
                        self.received_descriptors.push_back(OwnedFd::from_raw_fd(descriptor));
                    }
                }
                header = libc::CMSG_NXTHDR(&raw const message, header);
            }
        }

        if received == 0 {
            return Err(std::io::ErrorKind::UnexpectedEof.into());
        }
        self.unread_bytes.extend(&buffer[..received]);
        Ok(())
    }

    fn read_bytes<const COUNT: usize>(&mut self) -> std::io::Result<[u8; COUNT]> {
        let mut bytes = [0u8; COUNT];
        self.read_into(&mut bytes)?;
        Ok(bytes)
    }

    fn read_into(&mut self, bytes: &mut [u8]) -> std::io::Result<()> {
        while self.unread_bytes.len() < bytes.len() {
            self.receive_more()?;
        }
        for byte in bytes {
            *byte = self.unread_bytes.pop_front().expect("enough bytes were received");
        }
        Ok(())
    }

    fn read_vector(&mut self, length: usize) -> std::io::Result<Vec<u8>> {
        let mut bytes = vec![0u8; length];
        self.read_into(&mut bytes)?;
        Ok(bytes)
    }

    fn read_message(&mut self) -> std::io::Result<ControlMessage> {
        let invalid_data = || std::io::Error::from(std::io::ErrorKind::InvalidData);
        match self.read_bytes::<1>()?[0] {
            SOURCE_MESSAGE => {
                let agent_index = u32::from_le_bytes(self.read_bytes()?);
                let length = u32::from_le_bytes(self.read_bytes()?);
                let source = self.read_vector(length as usize)?;
                Ok(ControlMessage::Source { agent_index, source })
            }
            BROADCAST_MESSAGE => {
                let byte_length =
                    usize::try_from(u64::from_le_bytes(self.read_bytes()?)).map_err(|_| invalid_data())?;
                let object_id = u64::from_le_bytes(self.read_bytes()?);
                let number = match self.read_bytes::<1>()?[0] {
                    UNDEFINED_BROADCAST_NUMBER => BroadcastNumber::Undefined,
                    NUMBER_BROADCAST_NUMBER => {
                        BroadcastNumber::Number(f64::from_bits(u64::from_le_bytes(self.read_bytes()?)))
                    }
                    BIGINT_BROADCAST_NUMBER => {
                        let length = u32::from_le_bytes(self.read_bytes()?);
                        BroadcastNumber::BigInt(self.read_vector(length as usize)?)
                    }
                    _ => return Err(invalid_data()),
                };
                // The descriptor came with the first byte of the message.
                let shared_memory = self.received_descriptors.pop_front().ok_or_else(invalid_data)?;
                Ok(ControlMessage::Broadcast(Broadcast {
                    shared_memory,
                    byte_length,
                    object_id,
                    number,
                }))
            }
            _ => Err(invalid_data()),
        }
    }
}

/// Splits a message into frames that each fit into one atomic write to the report pipe.
fn encode_frames(agent_index: u32, kind: u8, payload: &[u8]) -> Vec<Vec<u8>> {
    let mut chunks: Vec<&[u8]> = payload.chunks(FRAME_PAYLOAD_LIMIT).collect();
    if chunks.is_empty() {
        chunks.push(&[]);
    }
    let last_index = chunks.len() - 1;
    chunks
        .into_iter()
        .enumerate()
        .map(|(index, chunk)| {
            let mut frame = Vec::with_capacity(FRAME_HEADER_SIZE + chunk.len());
            frame.extend_from_slice(&agent_index.to_le_bytes());
            frame.push(kind);
            frame.push(u8::from(index == last_index));
            frame.extend_from_slice(&u16::try_from(chunk.len()).expect("a frame fits in u16").to_le_bytes());
            frame.extend_from_slice(chunk);
            frame
        })
        .collect()
}

fn encode_uncaught_exception(name: &str, message: &str) -> Vec<u8> {
    let mut payload = u32::try_from(name.len()).unwrap_or(u32::MAX).to_le_bytes().to_vec();
    payload.extend_from_slice(name.as_bytes());
    payload.extend_from_slice(message.as_bytes());
    payload
}

fn decode_uncaught_exception(payload: &[u8]) -> AgentFailure {
    let name_length = payload
        .first_chunk::<4>()
        .map_or(0, |length| u32::from_le_bytes(*length) as usize)
        .min(payload.len().saturating_sub(4));
    let (name, message) = payload.get(4..).unwrap_or_default().split_at(name_length);
    AgentFailure::UncaughtException {
        name: String::from_utf8_lossy(name).into_owned(),
        message: String::from_utf8_lossy(message).into_owned(),
    }
}

/// Puts the frames that agents wrote to the report pipe back together into messages, in the order their last frames
/// arrived.
#[derive(Default)]
struct ReportAssembler {
    unparsed_bytes: Vec<u8>,
    partial_messages: HashMap<u32, Vec<u8>>,
    reports: VecDeque<Vec<u8>>,
    failures: Vec<AgentFailure>,
}

impl ReportAssembler {
    fn push_bytes(&mut self, bytes: &[u8]) {
        self.unparsed_bytes.extend_from_slice(bytes);
        let mut offset = 0;
        while let Some(header) = self.unparsed_bytes[offset..].first_chunk::<FRAME_HEADER_SIZE>() {
            let agent_index = u32::from_le_bytes([header[0], header[1], header[2], header[3]]);
            let kind = header[4];
            let is_last_frame = header[5] != 0;
            let payload_length = usize::from(u16::from_le_bytes([header[6], header[7]]));
            let frame_end = offset + FRAME_HEADER_SIZE + payload_length;
            if frame_end > self.unparsed_bytes.len() {
                break;
            }
            let payload = &self.unparsed_bytes[offset + FRAME_HEADER_SIZE..frame_end];
            let message = self.partial_messages.entry(agent_index).or_default();
            message.extend_from_slice(payload);
            if is_last_frame {
                let message = self.partial_messages.remove(&agent_index).unwrap_or_default();
                match kind {
                    REPORT_FRAME => self.reports.push_back(message),
                    UNCAUGHT_EXCEPTION_FRAME => self.failures.push(decode_uncaught_exception(&message)),
                    _ => self.failures.push(AgentFailure::Panic(format!(
                        "Agent {agent_index} panicked: {}",
                        String::from_utf8_lossy(&message)
                    ))),
                }
            }
            offset = frame_end;
        }
        self.unparsed_bytes.drain(..offset);
    }
}

struct RunningAgent {
    process: Child,
    socket: UnixStream,
    exit_was_reported: bool,
}

impl RunningAgent {
    fn wait_for_acknowledgement(&mut self, acknowledgement: u8) -> std::io::Result<()> {
        loop {
            let mut byte = [0u8; 1];
            self.socket.read_exact(&mut byte)?;
            if byte[0] == acknowledgement {
                return Ok(());
            }
        }
    }

    /// Why the agent is gone, if it went in a way that it could not report.
    fn unreported_exit(&mut self, agent_index: usize) -> Option<AgentFailure> {
        if self.exit_was_reported {
            return None;
        }
        let status = self.process.try_wait().ok()??;
        if status.success() {
            return None;
        }
        self.exit_was_reported = true;
        let description = match status.signal() {
            Some(signal) => format!("Agent {agent_index} was terminated by signal {signal}"),
            None => format!("Agent {agent_index} exited with {status}"),
        };
        Some(AgentFailure::Exited(description))
    }
}

/// The pipe that all agents of a test write their reports to.
struct ReportPipe {
    reader: File,
    writer: OwnedFd,
    assembler: ReportAssembler,
}

impl ReportPipe {
    fn create() -> std::io::Result<Self> {
        let (reader, writer) = std::io::pipe()?;
        let reader = File::from(OwnedFd::from(reader));
        // SAFETY: Only changes the status flags of the pipe's read end.
        unsafe {
            let flags = libc::fcntl(reader.as_raw_fd(), libc::F_GETFL);
            if flags < 0 || libc::fcntl(reader.as_raw_fd(), libc::F_SETFL, flags | libc::O_NONBLOCK) < 0 {
                return Err(std::io::Error::last_os_error());
            }
        }
        Ok(Self {
            reader,
            writer: writer.into(),
            assembler: ReportAssembler::default(),
        })
    }

    fn read_available_frames(&mut self) {
        let mut buffer = [0u8; 4096];
        loop {
            match self.reader.read(&mut buffer) {
                Ok(0) => return,
                Ok(count) => self.assembler.push_bytes(&buffer[..count]),
                Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
                Err(_) => return,
            }
        }
    }
}

/// The agents that the running test started.
#[derive(Default)]
struct RunningAgents {
    agents: Vec<RunningAgent>,
    report_pipe: Option<ReportPipe>,
    /// How many of the failures in the report pipe getReport() has thrown.
    thrown_failure_count: usize,
}

impl RunningAgents {
    fn start(&mut self, launcher: &AgentLauncher, source: &[u8]) -> std::io::Result<()> {
        let report_pipe = match &mut self.report_pipe {
            Some(report_pipe) => report_pipe,
            None => self.report_pipe.insert(ReportPipe::create()?),
        };
        let (test_socket, agent_socket) = UnixStream::pair()?;
        never_raise_sigpipe(&test_socket)?;
        let agent_index = u32::try_from(self.agents.len()).expect("a test starts fewer than 2^32 agents");
        let process = Command::new(&launcher.program)
            .args(&launcher.arguments)
            .stdin(Stdio::from(OwnedFd::from(agent_socket)))
            .stdout(Stdio::from(report_pipe.writer.try_clone()?))
            .stderr(Stdio::inherit())
            .spawn()?;
        self.agents.push(RunningAgent {
            process,
            socket: test_socket,
            exit_was_reported: false,
        });
        let agent = self.agents.last_mut().expect("the agent was just added");
        send_all(agent.socket.as_fd(), &encode_source_message(agent_index, source))?;
        agent.wait_for_acknowledgement(AGENT_STARTED_ACKNOWLEDGEMENT)
    }

    fn broadcast(
        &mut self,
        shared_memory: BorrowedFd<'_>,
        byte_length: usize,
        object_id: u64,
        number: &BroadcastNumber,
    ) {
        let message = encode_broadcast_message(byte_length, object_id, number);
        let mut receiving_agents = Vec::new();
        for (index, agent) in self.agents.iter().enumerate() {
            // An agent that already exited cannot take the message, and the test cannot expect it to.
            if send_with_descriptor(agent.socket.as_fd(), &message, shared_memory).is_ok() {
                receiving_agents.push(index);
            }
        }
        for index in receiving_agents {
            let _ = self.agents[index].wait_for_acknowledgement(BROADCAST_RECEIVED_ACKNOWLEDGEMENT);
        }
    }

    fn take_report(&mut self) -> Result<Option<Vec<u8>>, AgentFailure> {
        let Some(report_pipe) = &mut self.report_pipe else {
            return Ok(None);
        };
        report_pipe.read_available_frames();
        if let Some(report) = report_pipe.assembler.reports.pop_front() {
            return Ok(Some(report));
        }

        // With nothing to report, tell the test whether an agent failed, which would leave it waiting forever.
        let unreported_exits: Vec<AgentFailure> = self
            .agents
            .iter_mut()
            .enumerate()
            .filter_map(|(index, agent)| agent.unreported_exit(index))
            .collect();
        // What an agent wrote before it exited is in the pipe by now, and comes before its exit.
        report_pipe.read_available_frames();
        if let Some(report) = report_pipe.assembler.reports.pop_front() {
            return Ok(Some(report));
        }
        report_pipe.assembler.failures.extend(unreported_exits);
        let Some(failure) = report_pipe.assembler.failures.get(self.thrown_failure_count) else {
            return Ok(None);
        };
        self.thrown_failure_count += 1;
        Err(failure.clone())
    }
}

/// $262.agent.start(): runs `source`, the WTF-8 of a script, in a new agent and returns once the agent is running.
pub fn start_agent(source: &[u8]) -> Result<(), String> {
    AGENT_LAUNCHER.with_borrow(|launcher| {
        let launcher = launcher.as_ref().ok_or("Agents are unavailable")?;
        RUNNING_AGENTS
            .with_borrow_mut(|running_agents| running_agents.start(launcher, source))
            .map_err(|error| format!("Could not start an agent: {error}"))
    })
}

/// $262.agent.broadcast(): hands a SharedArrayBuffer's shared memory and `number` to every running agent, and returns
/// once each has them.
pub fn broadcast(shared_memory: BorrowedFd<'_>, byte_length: usize, object_id: u64, number: &BroadcastNumber) {
    RUNNING_AGENTS.with_borrow_mut(|running_agents| {
        running_agents.broadcast(shared_memory, byte_length, object_id, number);
    });
}

/// $262.agent.getReport(): the next report of any agent, which is WTF-8, or None if there is none yet. Fails instead of
/// returning None when an agent stopped short, as no report might come from it.
pub fn take_report() -> Result<Option<Vec<u8>>, AgentFailure> {
    RUNNING_AGENTS.with_borrow_mut(RunningAgents::take_report)
}

/// Ends the agents of the test that ran last, and returns how each one that failed did, whether getReport() threw it
/// or not.
pub fn terminate_agents() -> Vec<AgentFailure> {
    let running_agents = RUNNING_AGENTS.take();
    let failures = running_agents
        .report_pipe
        .map(|mut report_pipe| {
            report_pipe.read_available_frames();
            report_pipe.assembler.failures
        })
        .unwrap_or_default();
    for mut agent in running_agents.agents {
        let _ = agent.process.kill();
        let _ = agent.process.wait();
    }
    failures
}

/// An agent's side of the connection to the test that started it.
struct TestConnection {
    agent_index: u32,
    socket: OwnedFd,
    report_pipe: File,
    broadcasts: Mutex<VecDeque<Broadcast>>,
    broadcast_arrived: Condvar,
}

static TEST_CONNECTION: OnceLock<TestConnection> = OnceLock::new();

/// Moves a descriptor that the test handed down out of the way of the standard streams, and puts /dev/null there.
fn take_inherited_descriptor(descriptor: RawFd) -> std::io::Result<OwnedFd> {
    // SAFETY: Duplicates a descriptor this process inherited; the duplicate is owned here.
    let duplicate = unsafe { libc::fcntl(descriptor, libc::F_DUPFD_CLOEXEC, 3) };
    if duplicate < 0 {
        return Err(std::io::Error::last_os_error());
    }
    // SAFETY: fcntl() just created the descriptor.
    let duplicate = unsafe { OwnedFd::from_raw_fd(duplicate) };
    let null = File::options().read(true).write(true).open("/dev/null")?;
    // SAFETY: Replaces the inherited descriptor, which nothing else owns.
    if unsafe { libc::dup2(null.as_raw_fd(), descriptor) } < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(duplicate)
}

/// Agent mode: connects to the test that started this process, which passed it the socket as standard input and the
/// report pipe as standard output, and returns the WTF-8 source of the agent's script.
pub fn connect_to_test() -> std::io::Result<Vec<u8>> {
    let socket = take_inherited_descriptor(libc::STDIN_FILENO)?;
    let report_pipe = take_inherited_descriptor(libc::STDOUT_FILENO)?;
    let mut reader = ControlReader::new(socket.try_clone()?);
    let ControlMessage::Source { agent_index, source } = reader.read_message()? else {
        return Err(std::io::ErrorKind::InvalidData.into());
    };
    let connection = TestConnection {
        agent_index,
        socket,
        report_pipe: report_pipe.into(),
        broadcasts: Mutex::new(VecDeque::new()),
        broadcast_arrived: Condvar::new(),
    };
    if TEST_CONNECTION.set(connection).is_err() {
        return Err(std::io::ErrorKind::AlreadyExists.into());
    }
    std::thread::Builder::new()
        .name("Agent control".to_string())
        .spawn(move || receive_broadcasts(reader))?;
    Ok(source)
}

fn test_connection() -> &'static TestConnection {
    TEST_CONNECTION.get().expect("agent mode connected to the test")
}

fn receive_broadcasts(mut reader: ControlReader) {
    let connection = test_connection();
    while let Ok(ControlMessage::Broadcast(broadcast)) = reader.read_message() {
        connection
            .broadcasts
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .push_back(broadcast);
        connection.broadcast_arrived.notify_all();
        if send_all(connection.socket.as_fd(), &[BROADCAST_RECEIVED_ACKNOWLEDGEMENT]).is_err() {
            break;
        }
    }
    // SAFETY: The test is gone, or no longer speaks the protocol, so nobody waits for this agent any more. Exiting
    //         from here also stops a main thread that is waiting or running forever.
    unsafe { libc::_exit(0) }
}

/// Tells the test that this agent is running, which ends $262.agent.start().
pub fn acknowledge_start() {
    let _ = send_all(test_connection().socket.as_fd(), &[AGENT_STARTED_ACKNOWLEDGEMENT]);
}

/// $262.agent.receiveBroadcast(): waits for the next broadcast.
pub fn receive_broadcast() -> Broadcast {
    let connection = test_connection();
    let mut broadcasts = connection.broadcasts.lock().unwrap_or_else(PoisonError::into_inner);
    loop {
        if let Some(broadcast) = broadcasts.pop_front() {
            return broadcast;
        }
        broadcasts = connection
            .broadcast_arrived
            .wait(broadcasts)
            .unwrap_or_else(PoisonError::into_inner);
    }
}

fn write_to_report_pipe(kind: u8, payload: &[u8]) {
    let connection = test_connection();
    for frame in encode_frames(connection.agent_index, kind, payload) {
        // A report that cannot be written has nobody left to read it.
        let _ = (&connection.report_pipe).write_all(&frame);
    }
}

/// $262.agent.report(), with the WTF-8 of the reported string.
pub fn report(message: &[u8]) {
    write_to_report_pipe(REPORT_FRAME, message);
}

/// Tells the test that the agent's script threw, or left a promise rejected without a handler.
pub fn report_uncaught_exception(name: &str, message: &str) {
    write_to_report_pipe(UNCAUGHT_EXCEPTION_FRAME, &encode_uncaught_exception(name, message));
}

/// Tells the test that the agent panicked, as a failed assertion in the runtime does.
pub fn report_panic(message: &str) {
    write_to_report_pipe(PANIC_FRAME, message.as_bytes());
}
