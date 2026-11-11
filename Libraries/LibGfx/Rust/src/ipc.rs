/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Codecs for the Gfx values carried by IPC.

use crate::{Color, FloatPoint, FloatRect, IntPoint, IntRect, IntSize};
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

impl Encode for IntPoint {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x)?;
        encoder.encode(&self.y)
    }
}

impl Decode for IntPoint {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<i32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
        })
    }
}

impl Encode for FloatPoint {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x)?;
        encoder.encode(&self.y)
    }
}

impl Decode for FloatPoint {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<f32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
        })
    }
}

impl Encode for IntSize {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.width)?;
        encoder.encode(&self.height)
    }
}

impl Decode for IntSize {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<i32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            width: decoder.decode()?,
            height: decoder.decode()?,
        })
    }
}

impl Encode for IntRect {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x)?;
        encoder.encode(&self.y)?;
        encoder.encode(&self.width)?;
        encoder.encode(&self.height)
    }
}

impl Decode for IntRect {
    const MIN_ENCODED_SIZE: usize = 4 * size_of::<i32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
            width: decoder.decode()?,
            height: decoder.decode()?,
        })
    }
}

impl Encode for FloatRect {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x)?;
        encoder.encode(&self.y)?;
        encoder.encode(&self.width)?;
        encoder.encode(&self.height)
    }
}

impl Decode for FloatRect {
    const MIN_ENCODED_SIZE: usize = 4 * size_of::<f32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
            width: decoder.decode()?,
            height: decoder.decode()?,
        })
    }
}

impl Encode for Color {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.0)
    }
}

impl Decode for Color {
    const MIN_ENCODED_SIZE: usize = size_of::<u32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self(decoder.decode()?))
    }
}
