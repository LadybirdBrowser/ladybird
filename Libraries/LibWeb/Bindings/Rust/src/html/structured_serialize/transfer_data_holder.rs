/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The data holders of the transfer steps and the transfer-receiving steps,
//! https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects

use core::cell::RefCell;
use core::ffi::{c_int, c_void};
use libipc_rust::{Attachment, Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

#[repr(C)]
pub(crate) struct RawAttachment {
    value: c_int,
    disposition: u32,
}

impl RawAttachment {
    /// # Safety
    ///
    /// The raw attachment must hand over one owned descriptor or Mach right.
    unsafe fn into_attachment(self) -> Attachment {
        #[cfg(target_vendor = "apple")]
        {
            use libipc_rust::mach::{
                MACH_MSG_TYPE_MAKE_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE, MACH_MSG_TYPE_MOVE_RECEIVE,
                MACH_MSG_TYPE_MOVE_SEND_ONCE, MachPort, PortRight,
            };
            let right = match self.disposition {
                MACH_MSG_TYPE_MOVE_RECEIVE | MACH_MSG_TYPE_MAKE_SEND | MACH_MSG_TYPE_MAKE_SEND_ONCE => {
                    PortRight::Receive
                }
                MACH_MSG_TYPE_MOVE_SEND_ONCE => PortRight::SendOnce,
                _ => PortRight::Send,
            };
            Attachment::from_mach_port(MachPort::adopt(self.value as u32, right), self.disposition)
        }
        #[cfg(all(unix, not(target_vendor = "apple")))]
        {
            use std::os::fd::FromRawFd;
            Attachment::from(unsafe { std::os::fd::OwnedFd::from_raw_fd(self.value) })
        }
        #[cfg(windows)]
        {
            Attachment::from_descriptor(unsafe {
                libipc_rust::transport_windows::OwnedDescriptor::from_raw(self.value)
            })
        }
    }

    /// # Safety
    ///
    /// `raw` must point to `count` raw attachments, each of which hands over its descriptor or Mach right.
    pub(crate) unsafe fn take_all(raw: *const Self, count: usize) -> Vec<Attachment> {
        // SAFETY: The caller lends the raw attachments and hands over what each one names.
        unsafe { ak::slice_from_raw_parts(raw, count) }
            .iter()
            .map(|attachment| {
                let owned = Self {
                    value: attachment.value,
                    disposition: attachment.disposition,
                };
                unsafe { owned.into_attachment() }
            })
            .collect()
    }

    pub(crate) fn from_attachment(attachment: Attachment) -> Self {
        #[cfg(target_vendor = "apple")]
        {
            let (port, disposition) = attachment.into_mach_port();
            Self {
                value: core::mem::ManuallyDrop::new(port).name() as c_int,
                disposition,
            }
        }
        #[cfg(all(unix, not(target_vendor = "apple")))]
        {
            use std::os::fd::IntoRawFd;
            Self {
                value: attachment
                    .into_fd()
                    .expect("a Unix attachment is a descriptor")
                    .into_raw_fd(),
                disposition: 0,
            }
        }
        #[cfg(windows)]
        {
            Self {
                value: attachment.into_descriptor().into_raw(),
                disposition: 0,
            }
        }
    }
}

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

pub(crate) struct TransferPayload {
    pub(crate) bytes: Vec<u8>,
    pub(crate) attachments: Vec<Attachment>,
}

impl TransferPayload {
    pub(crate) fn into_cpp(self) -> *mut c_void {
        self.with_raw(|bytes, attachments| {
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
        payload.expect("C++ hands over the payload")
    }

    pub(crate) unsafe extern "C" fn receive(
        context: *mut c_void,
        bytes: ak::Span<'_, u8>,
        attachments: *const RawAttachment,
        count: usize,
    ) {
        // SAFETY: The caller lends this result slot and bytes, and transfers every raw attachment.
        let attachments = unsafe { RawAttachment::take_all(attachments, count) };
        unsafe {
            *context.cast::<Option<Self>>() = Some(Self {
                bytes: bytes.as_slice().to_vec(),
                attachments,
            });
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
        Ok(Self { bytes, attachments })
    }
}

pub(crate) struct TransferDataHolder(RefCell<Encoder>);

impl TransferDataHolder {
    pub(crate) fn new() -> Self {
        Self(RefCell::new(Encoder::new()))
    }

    pub(crate) fn encode<T: Encode + ?Sized>(&self, value: &T) {
        self.with_encoder(|encoder| encoder.encode(value));
    }

    pub(crate) fn with_encoder(&self, write: impl FnOnce(&mut Encoder) -> Result<(), EncodeError>) {
        write(&mut self.0.borrow_mut()).expect("a data holder encodes what it is given");
    }

    pub(crate) fn extend(&self, payload: TransferPayload) {
        let mut encoder = self.0.borrow_mut();
        encoder.append_bytes(&payload.bytes);
        append_attachments(&mut encoder, payload.attachments);
    }

    pub(crate) fn finish(self) -> TransferPayload {
        let (bytes, attachments) = self.0.into_inner().finish();
        TransferPayload { bytes, attachments }
    }
}
