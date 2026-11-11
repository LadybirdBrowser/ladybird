/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types the Compositor's endpoints carry, under the names the IPC compiler gives them: the ones it generates from
//! their .ipc declarations, the ones with a wire form of their own, and the endpoints it generates from
//! Services/Compositor/*.ipc for the messages that name Rust.

#![allow(non_camel_case_types)]

use crate::css_pixels::{CssPixelPoint, CssPixels};
use crate::display_list::commands::{CompositorContextId, UniqueNodeId};
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

pub type Web_CompositorContextId = CompositorContextId;
pub type Web_UniqueNodeID = UniqueNodeId;
pub type Web_CSSPixelPoint = CssPixelPoint;
pub type Gfx_IntPoint = libgfx_rust::IntPoint;
pub type Gfx_FloatPoint = libgfx_rust::FloatPoint;
pub type Gfx_IntSize = libgfx_rust::IntSize;
pub type Gfx_IntRect = libgfx_rust::IntRect;
pub type Gfx_FloatRect = libgfx_rust::FloatRect;
pub type Gfx_Color = libgfx_rust::Color;

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

#[allow(non_upper_case_globals)]
impl Web_UIEvents_KeyModifier {
    pub const Mod_PlatformCtrl: Self = if cfg!(target_os = "macos") {
        Self::Mod_Super
    } else {
        Self::Mod_Ctrl
    };
}

impl Web_InputRoutingDecision {
    // The name tests read a decision by. ENUMERATE_INPUT_ROUTING_DECISIONS gives each decision its name in kebab case.
    pub fn name(self) -> String {
        let mut name = String::new();
        for character in format!("{self:?}").chars() {
            if character.is_ascii_uppercase() && !name.is_empty() {
                name.push('-');
            }
            name.push(character.to_ascii_lowercase());
        }
        name
    }
}

include!(concat!(env!("OUT_DIR"), "/AsyncScrollNodeStableID.rs"));
include!(concat!(env!("OUT_DIR"), "/DisplayListProperties.rs"));
include!(concat!(env!("OUT_DIR"), "/InputEvent.rs"));
include!(concat!(env!("OUT_DIR"), "/InputRoutingDecision.rs"));
include!(concat!(env!("OUT_DIR"), "/InputTrace.rs"));
include!(concat!(env!("OUT_DIR"), "/KeyCode.rs"));
include!(concat!(env!("OUT_DIR"), "/MouseButton.rs"));
include!(concat!(env!("OUT_DIR"), "/TextEditingCommand.rs"));
include!(concat!(env!("OUT_DIR"), "/Types.rs"));

include!(concat!(env!("OUT_DIR"), "/CompositorPresentationClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorPresentationServerEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorWebContentClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorWebContentServerEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/RenderClockClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/RenderClockServerEndpoint.rs"));
