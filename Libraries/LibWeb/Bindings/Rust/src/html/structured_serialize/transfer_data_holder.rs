/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The data holder of a transferred value, in the encoding of IPC, which only IPC carries. Rust writes its bytes. The
//! values that have attachments, like the end of a transport or the records of messages, C++ encodes into the holder's
//! carrier, after the bytes before them.

use core::cell::RefCell;

use libipc_rust::{Encode, Encoder};

use crate::html::structured_serialize::cpp::TransferDataCarrier;

pub(crate) struct TransferDataHolder {
    /// The bytes since the carrier last took them.
    bytes: RefCell<Encoder>,
    carrier: TransferDataCarrier,
}

impl TransferDataHolder {
    pub(crate) fn new() -> Self {
        Self {
            bytes: RefCell::new(Encoder::new()),
            carrier: TransferDataCarrier::new(),
        }
    }

    pub(crate) fn encode<T: Encode + ?Sized>(&self, value: &T) {
        self.bytes
            .borrow_mut()
            .encode(value)
            .expect("a data holder encodes what it is given");
    }

    /// The carrier, with the bytes so far, for C++ to encode a value that has attachments into.
    pub(crate) fn carrier(&self) -> &TransferDataCarrier {
        let (bytes, attachments) = core::mem::take(&mut *self.bytes.borrow_mut()).finish();
        debug_assert!(attachments.is_empty(), "C++ encodes the values that have attachments");
        self.carrier.append(&bytes);
        &self.carrier
    }

    /// The carrier, with every byte of the holder, which C++ takes over.
    pub(crate) fn into_carrier(self) -> TransferDataCarrier {
        self.carrier();
        self.carrier
    }
}
