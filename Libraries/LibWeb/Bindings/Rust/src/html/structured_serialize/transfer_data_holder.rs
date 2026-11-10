/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The data holders of the transfer steps and the transfer-receiving steps,
//! https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects

use core::cell::RefCell;
use core::ffi::c_void;
pub(crate) use libipc_rust::cpp_connection::RawAttachment;
use libipc_rust::{Attachment, Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};
use std::collections::VecDeque;

use crate::html::message_port::TransportHandle;
use crate::html::port_channel::{PortChannelEnd, claim_parked_end, park_end};
use crate::html::structured_serialize::transfer_record::PortMessage;

pub(crate) type ReceivePayload = unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>, *const RawAttachment, usize);

unsafe extern "C" {
    fn web_structured_serialize_transfer_data_holder_create(
        bytes: ak::Span<'_, u8>,
        attachments: *const RawAttachment,
        count: usize,
    ) -> *mut c_void;
    fn web_structured_serialize_transfer_data_holder_take(
        holder: *mut c_void,
        context: *mut c_void,
        receive: ReceivePayload,
    );
}

pub(crate) fn append_attachments(encoder: &mut Encoder, attachments: Vec<Attachment>) {
    for attachment in attachments {
        encoder.append_attachment(attachment);
    }
}

/// A channel end that a data holder carries.
pub(crate) enum CarriedChannelEnd {
    Held(PortChannelEnd),
    /// Parked in this process, under the token, for the data holder to go through the browser process. The transport is
    /// for a data holder that may arrive in another process.
    Parked(u64, Option<TransportHandle>),
}

impl CarriedChannelEnd {
    /// A data holder that is encoded leaves the process, as far as this end knows, so the end changes over to a transport.
    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let (token, messages, transport) = match self {
            Self::Held(end) => {
                let (messages, transport) = end.leave_process();
                (0, messages, transport)
            }
            Self::Parked(token, transport) => (token, Vec::new(), transport),
        };
        encoder.encode(&token)?;
        encoder.encode_size(messages.len())?;
        for message in messages {
            message.encode_into(encoder)?;
        }
        encoder.encode(&transport.is_some())?;
        if let Some(transport) = transport {
            transport.encode_into(encoder)?;
        }
        Ok(())
    }

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let token = decoder.decode::<u64>()?;
        let count = decoder.decode_size()?;
        let mut messages = Vec::new();
        for _ in 0..count {
            messages.push(PortMessage::decode(decoder)?);
        }
        let transport = decoder
            .decode::<bool>()?
            .then(|| TransportHandle::decode(decoder))
            .transpose()?;
        if token != 0
            && let Some(end) = claim_parked_end(token)
        {
            return Ok(Self::Held(end));
        }
        Ok(Self::Held(PortChannelEnd::from_elsewhere(messages, transport)))
    }

    pub(crate) fn into_end(self) -> PortChannelEnd {
        match self {
            Self::Held(end) => end,
            Self::Parked(..) => unreachable!("a parked end is only ever encoded"),
        }
    }
}

pub(crate) struct TransferPayload {
    pub(crate) bytes: Vec<u8>,
    pub(crate) attachments: Vec<Attachment>,
    /// The channel ends of the MessagePorts that the data holder carries, which its readers take in the order its writers
    /// added them.
    pub(crate) channel_ends: VecDeque<CarriedChannelEnd>,
}

impl TransferPayload {
    pub(crate) fn new(bytes: Vec<u8>, attachments: Vec<Attachment>) -> Self {
        Self {
            bytes,
            attachments,
            channel_ends: VecDeque::new(),
        }
    }

    /// Parks the channel end of a data holder that goes through the browser process to a thread of a worker, and returns
    /// the token that settling it takes. This is for a holder with one end, of a new channel.
    pub(crate) fn park_channel_end(&mut self, may_leave_process: bool) -> u64 {
        let end = self
            .channel_ends
            .pop_front()
            .expect("the data holder of a new channel holds its end")
            .into_end();
        let (token, transport) = park_end(end, may_leave_process);
        self.channel_ends.push_back(CarriedChannelEnd::Parked(token, transport));
        token
    }

    /// The carrier of this data holder for C++, for which it is opaque. Encoding makes it fit to leave the process.
    pub(crate) fn into_cpp(self) -> *mut c_void {
        let mut encoder = Encoder::new();
        self.encode_owned(&mut encoder).expect("a data holder encodes");
        let (bytes, attachments) = encoder.finish();
        Self::new(bytes, attachments).with_raw(|bytes, attachments| {
            // SAFETY: C++ copies the lent bytes and takes all attachments.
            unsafe {
                web_structured_serialize_transfer_data_holder_create(bytes, attachments.as_ptr(), attachments.len())
            }
        })
    }

    /// # Safety
    ///
    /// C++ must hand over the holder and its attachments.
    pub(crate) unsafe fn from_cpp(holder: *mut c_void) -> Self {
        let mut payload = None;
        // SAFETY: The holder is handed over, and the callback fills the lent result slot.
        unsafe { web_structured_serialize_transfer_data_holder_take(holder, (&raw mut payload).cast(), Self::receive) };
        let payload: Self = payload.expect("C++ hands over the payload");
        // NB: What cannot be decoded holds nothing, which is what reading from it reports.
        let mut decoder = Decoder::new(&payload.bytes, payload.attachments);
        decoder
            .decode::<Self>()
            .unwrap_or_else(|_| Self::new(Vec::new(), Vec::new()))
    }

    pub(crate) unsafe extern "C" fn receive(
        context: *mut c_void,
        bytes: ak::Span<'_, u8>,
        attachments: *const RawAttachment,
        count: usize,
    ) {
        // SAFETY: The caller lends this result slot and bytes, and transfers every raw attachment.
        let attachments = unsafe { RawAttachment::take_all(ak::slice_from_raw_parts(attachments, count)) };
        unsafe {
            *context.cast::<Option<Self>>() = Some(Self::new(bytes.as_slice().to_vec(), attachments));
        };
    }

    pub(crate) fn with_raw<R>(self, steps: impl FnOnce(ak::Span<'_, u8>, &[RawAttachment]) -> R) -> R {
        let attachments: Vec<_> = self
            .attachments
            .into_iter()
            .map(RawAttachment::from_attachment)
            .collect();
        steps(ak::Span::new(&self.bytes), &attachments)
    }
}

impl Encode for TransferPayload {
    fn encode(&self, _: &mut Encoder) -> Result<(), EncodeError> {
        Err(EncodeError::InvalidValue(
            "a transfer payload must move its attachments",
        ))
    }

    fn encode_owned(self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(self.bytes.as_slice())?;
        encoder.encode_size(self.attachments.len())?;
        append_attachments(encoder, self.attachments);
        encoder.encode_size(self.channel_ends.len())?;
        for end in self.channel_ends {
            end.encode_owned(encoder)?;
        }
        Ok(())
    }
}

impl Decode for TransferPayload {
    const MIN_ENCODED_SIZE: usize = 8;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let bytes = decoder.decode::<Vec<u8>>()?;
        let count = decoder.decode::<u32>()? as usize;
        if count > decoder.remaining_attachments() {
            return Err(DecodeError::MissingAttachment);
        }
        let attachments = (0..count)
            .map(|_| decoder.take_attachment())
            .collect::<Result<_, _>>()?;
        let end_count = decoder.decode_size()?;
        let mut channel_ends = VecDeque::new();
        for _ in 0..end_count {
            channel_ends.push_back(CarriedChannelEnd::decode(decoder)?);
        }
        Ok(Self {
            bytes,
            attachments,
            channel_ends,
        })
    }
}

pub(crate) struct TransferDataHolder {
    encoder: RefCell<Encoder>,
    channel_ends: RefCell<VecDeque<CarriedChannelEnd>>,
}

impl TransferDataHolder {
    pub(crate) fn new() -> Self {
        Self {
            encoder: RefCell::new(Encoder::new()),
            channel_ends: RefCell::new(VecDeque::new()),
        }
    }

    pub(crate) fn encode<T: Encode + ?Sized>(&self, value: &T) {
        self.with_encoder(|encoder| encoder.encode(value));
    }

    pub(crate) fn with_encoder(&self, write: impl FnOnce(&mut Encoder) -> Result<(), EncodeError>) {
        write(&mut self.encoder.borrow_mut()).expect("a data holder encodes what it is given");
    }

    pub(crate) fn add_channel_end(&self, end: PortChannelEnd) {
        self.channel_ends.borrow_mut().push_back(CarriedChannelEnd::Held(end));
    }

    pub(crate) fn extend(&self, payload: TransferPayload) {
        let mut encoder = self.encoder.borrow_mut();
        encoder.append_bytes(&payload.bytes);
        append_attachments(&mut encoder, payload.attachments);
        self.channel_ends.borrow_mut().extend(payload.channel_ends);
    }

    pub(crate) fn finish(self) -> TransferPayload {
        let (bytes, attachments) = self.encoder.into_inner().finish();
        TransferPayload {
            bytes,
            attachments,
            channel_ends: self.channel_ends.into_inner(),
        }
    }
}
