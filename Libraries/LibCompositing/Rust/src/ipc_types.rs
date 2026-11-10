/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types the Compositor's endpoints carry, under the names the IPC compiler gives them, and the endpoints it
//! generates from Services/Compositor/*.ipc for the messages that name Rust.

#![allow(non_camel_case_types)]

use crate::css_pixels::{CssPixelPoint, CssPixels};
use crate::display_list::commands::{CompositorContextId, UniqueNodeId};
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

pub type Web_CompositorContextId = CompositorContextId;
pub type Web_UniqueNodeID = UniqueNodeId;
pub type Web_CSSPixelPoint = CssPixelPoint;

impl Encode for CompositorContextId {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.0)
    }
}

impl Decode for CompositorContextId {
    const MIN_ENCODED_SIZE: usize = size_of::<u64>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self(decoder.decode()?))
    }
}

impl Encode for UniqueNodeId {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.0)
    }
}

impl Decode for UniqueNodeId {
    const MIN_ENCODED_SIZE: usize = size_of::<i64>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self(decoder.decode()?))
    }
}

impl Encode for CssPixelPoint {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x.raw_value())?;
        encoder.encode(&self.y.raw_value())
    }
}

impl Decode for CssPixelPoint {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<i32>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: CssPixels::from_raw(decoder.decode()?),
            y: CssPixels::from_raw(decoder.decode()?),
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum Web_AsyncScrollNodeKind {
    Viewport,
    Element,
    PseudoElement,
    VisualViewport,
}

impl Encode for Web_AsyncScrollNodeKind {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&(*self as u8))
    }
}

impl Decode for Web_AsyncScrollNodeKind {
    const MIN_ENCODED_SIZE: usize = size_of::<u8>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        match decoder.decode::<u8>()? {
            0 => Ok(Self::Viewport),
            1 => Ok(Self::Element),
            2 => Ok(Self::PseudoElement),
            3 => Ok(Self::VisualViewport),
            _ => Err(DecodeError::InvalidValue("Web::AsyncScrollNodeKind")),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct Web_AsyncScrollNodeStableID {
    pub node_id: UniqueNodeId,
    pub kind: Web_AsyncScrollNodeKind,
    pub pseudo_element_type: u8,
}

impl Encode for Web_AsyncScrollNodeStableID {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.node_id)?;
        encoder.encode(&self.kind)?;
        encoder.encode(&self.pseudo_element_type)
    }
}

impl Decode for Web_AsyncScrollNodeStableID {
    const MIN_ENCODED_SIZE: usize =
        UniqueNodeId::MIN_ENCODED_SIZE + Web_AsyncScrollNodeKind::MIN_ENCODED_SIZE + size_of::<u8>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            node_id: decoder.decode()?,
            kind: decoder.decode()?,
            pseudo_element_type: decoder.decode()?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Web_CompositorScrollOffset {
    pub scroll_node: Web_AsyncScrollNodeStableID,
    pub offset: CssPixelPoint,
}

impl Encode for Web_CompositorScrollOffset {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.scroll_node)?;
        encoder.encode(&self.offset)
    }
}

impl Decode for Web_CompositorScrollOffset {
    const MIN_ENCODED_SIZE: usize = Web_AsyncScrollNodeStableID::MIN_ENCODED_SIZE + CssPixelPoint::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            scroll_node: decoder.decode()?,
            offset: decoder.decode()?,
        })
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Web_DevicePixelPoint {
    pub x: i32,
    pub y: i32,
}

impl Encode for Web_DevicePixelPoint {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x)?;
        encoder.encode(&self.y)
    }
}

impl Decode for Web_DevicePixelPoint {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<i32>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
        })
    }
}

include!(concat!(env!("OUT_DIR"), "/RenderClockClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/RenderClockServerEndpoint.rs"));
