/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CTAPHID protocol: messages to and from one device, in packets of the size of a HID report.

use super::{DeviceError, HidDevice, Host, REPORT_SIZE, Report};

pub const COMMAND_INIT: u8 = 0x06;
pub const COMMAND_CBOR: u8 = 0x10;
pub const COMMAND_CANCEL: u8 = 0x11;
pub const COMMAND_KEEP_ALIVE: u8 = 0x3b;
pub const COMMAND_ERROR: u8 = 0x3f;

pub const BROADCAST_CHANNEL: u32 = 0xffff_ffff;
pub const INITIALIZATION_PACKET_FLAG: u8 = 0x80;
pub const CAPABILITY_CBOR: u8 = 0x04;
pub const INITIALIZATION_PACKET_DATA_SIZE: usize = REPORT_SIZE - 7;
pub const CONTINUATION_PACKET_DATA_SIZE: usize = REPORT_SIZE - 5;
/// One initialization packet and the 128 continuation packets that the sequence number allows.
pub const MAXIMUM_PAYLOAD_SIZE: usize = INITIALIZATION_PACKET_DATA_SIZE + 128 * CONTINUATION_PACKET_DATA_SIZE;

pub const POLL_INTERVAL_IN_MILLISECONDS: u32 = 20;
/// How long a device has for an answer that needs no user.
pub const DEVICE_ANSWER_TIMEOUT_IN_MILLISECONDS: u64 = 1500;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChannelError {
    Device,
    /// The device did not answer in the time that it has for an answer without the user.
    NoAnswer,
    /// The device gave the broadcast channel for a channel of its own.
    BroadcastChannel,
    MessageTooLong,
    /// The device sent a CTAPHID_ERROR message.
    ErrorMessage,
    PacketOutOfSequence,
}

impl From<DeviceError> for ChannelError {
    fn from(_: DeviceError) -> Self {
        Self::Device
    }
}

#[derive(Debug, PartialEq, Eq)]
pub struct Message {
    pub command: u8,
    pub payload: Vec<u8>,
}

/// A message that the device is sending, of which more packets are to come.
struct Assembly {
    command: u8,
    expected_payload_size: usize,
    next_sequence: u8,
    payload: Vec<u8>,
}

/// A channel to one device.
pub struct Channel {
    device: Box<dyn HidDevice>,
    channel_id: u32,
    capabilities: u8,
    assembly: Option<Assembly>,
}

impl Channel {
    /// Gets a channel of its own from the device.
    pub fn open(device: Box<dyn HidDevice>, host: &mut dyn Host) -> Result<Self, ChannelError> {
        let mut channel = Self {
            device,
            channel_id: BROADCAST_CHANNEL,
            capabilities: 0,
            assembly: None,
        };

        // NB: The device gives a channel of its own to the client that sends CTAPHID_INIT on the broadcast channel,
        //     and repeats the nonce, so that the client knows its answer from that for another client.
        let mut nonce = [0; 8];
        host.fill_with_random(&mut nonce);
        channel.send(COMMAND_INIT, &nonce)?;

        // NB: A poll can end early, such as for a report of another channel, so this goes by the clock.
        let deadline = host.now_in_milliseconds() + DEVICE_ANSWER_TIMEOUT_IN_MILLISECONDS;
        while host.now_in_milliseconds() < deadline {
            let Some(message) = channel.poll(POLL_INTERVAL_IN_MILLISECONDS)? else {
                continue;
            };
            let payload = &message.payload;
            if message.command != COMMAND_INIT || payload.len() < 17 || payload[..8] != nonce {
                continue;
            }
            let channel_id = u32::from_be_bytes([payload[8], payload[9], payload[10], payload[11]]);
            // NB: Channel 0 is reserved as well.
            if channel_id == BROADCAST_CHANNEL || channel_id == 0 {
                return Err(ChannelError::BroadcastChannel);
            }
            channel.channel_id = channel_id;
            channel.capabilities = payload[16];
            return Ok(channel);
        }
        Err(ChannelError::NoAnswer)
    }

    pub fn device_id(&self) -> u64 {
        self.device.id()
    }

    pub fn supports_cbor(&self) -> bool {
        self.capabilities & CAPABILITY_CBOR != 0
    }

    fn packet(&self, first_byte: u8) -> Report {
        let mut packet = [0; REPORT_SIZE];
        packet[..4].copy_from_slice(&self.channel_id.to_be_bytes());
        packet[4] = first_byte;
        packet
    }

    pub fn send(&mut self, command: u8, payload: &[u8]) -> Result<(), ChannelError> {
        if payload.len() > MAXIMUM_PAYLOAD_SIZE {
            return Err(ChannelError::MessageTooLong);
        }
        // NB: A new request ends what the device was still sending for the one before it.
        self.assembly = None;

        let (first, mut rest) = payload.split_at(payload.len().min(INITIALIZATION_PACKET_DATA_SIZE));
        let mut packet = self.packet(command | INITIALIZATION_PACKET_FLAG);
        packet[5..7].copy_from_slice(&(payload.len() as u16).to_be_bytes());
        packet[7..7 + first.len()].copy_from_slice(first);
        self.device.write_report(&packet)?;

        let mut sequence = 0;
        while !rest.is_empty() {
            let (chunk, remaining) = rest.split_at(rest.len().min(CONTINUATION_PACKET_DATA_SIZE));
            let mut packet = self.packet(sequence);
            packet[5..5 + chunk.len()].copy_from_slice(chunk);
            self.device.write_report(&packet)?;
            sequence += 1;
            rest = remaining;
        }
        Ok(())
    }

    /// The message that the device sent, when the time gave all of its packets.
    pub fn poll(&mut self, timeout_in_milliseconds: u32) -> Result<Option<Message>, ChannelError> {
        let Some(report) = self.device.read_report(timeout_in_milliseconds)? else {
            return Ok(None);
        };

        // NB: The device also sends the answers for the other clients of the host.
        if report[..4] != self.channel_id.to_be_bytes() {
            return Ok(None);
        }

        if report[4] & INITIALIZATION_PACKET_FLAG != 0 {
            let command = report[4] & !INITIALIZATION_PACKET_FLAG;
            let payload_size = usize::from(u16::from_be_bytes([report[5], report[6]]));
            // NB: The device says with these that it still works on the request, or waits for the user.
            if command == COMMAND_KEEP_ALIVE {
                return Ok(None);
            }
            if command == COMMAND_ERROR {
                return Err(ChannelError::ErrorMessage);
            }
            if payload_size > MAXIMUM_PAYLOAD_SIZE {
                return Err(ChannelError::MessageTooLong);
            }
            let data = &report[7..7 + payload_size.min(INITIALIZATION_PACKET_DATA_SIZE)];
            self.assembly = Some(Assembly {
                command,
                expected_payload_size: payload_size,
                next_sequence: 0,
                payload: data.to_vec(),
            });
        } else {
            let Some(assembly) = &mut self.assembly else {
                return Ok(None);
            };
            if report[4] != assembly.next_sequence {
                self.assembly = None;
                return Err(ChannelError::PacketOutOfSequence);
            }
            assembly.next_sequence += 1;
            let remaining = assembly.expected_payload_size - assembly.payload.len();
            assembly
                .payload
                .extend_from_slice(&report[5..5 + remaining.min(CONTINUATION_PACKET_DATA_SIZE)]);
        }

        if self
            .assembly
            .as_ref()
            .is_some_and(|assembly| assembly.payload.len() < assembly.expected_payload_size)
        {
            return Ok(None);
        }
        Ok(self.assembly.take().map(|assembly| Message {
            command: assembly.command,
            payload: assembly.payload,
        }))
    }

    /// Tells the device to stop waiting for the user. It still answers the request, with an error.
    pub fn cancel(&mut self) {
        let packet = self.packet(COMMAND_CANCEL | INITIALIZATION_PACKET_FLAG);
        // NB: A device that is gone needs no telling.
        let _ = self.device.write_report(&packet);
    }
}
