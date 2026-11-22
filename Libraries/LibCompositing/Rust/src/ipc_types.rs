/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types the Compositor's endpoints carry, under the names the IPC compiler gives them: the ones it generates from
//! their .ipc declarations, the ones with a wire form of their own, and the endpoints it generates from
//! Services/Compositor/*.ipc for the messages that name Rust.

#![allow(non_camel_case_types)]

use crate::css_pixels::CssPixelPoint;
use crate::display_list::chunks::ChunkId;
use crate::display_list::commands::{CanvasId, CompositorContextId, UniqueNodeId};
use crate::display_list::layers::{LayerPlan, UnvalidatedLayerPlan};
use crate::display_list::storage::PackedChunkSize;
use crate::visual_context::VisualContextTree;
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder, File};
use std::sync::Arc;

pub type Web_CompositorContextId = CompositorContextId;
pub type Compositing_CanvasId = CanvasId;
pub type Compositing_WebGL_ImageSource = crate::webgl::ImageSource;
pub type Gfx_AlphaType = crate::display_list::canvas_2d_recorder::AlphaType;
pub type Web_UniqueNodeID = UniqueNodeId;
pub type Web_CSSPixelPoint = CssPixelPoint;
pub type Gfx_IntPoint = libgfx_rust::IntPoint;
pub type Gfx_FloatPoint = libgfx_rust::FloatPoint;
pub type Gfx_IntSize = libgfx_rust::IntSize;
pub type Gfx_IntRect = libgfx_rust::IntRect;
pub type Gfx_FloatRect = libgfx_rust::FloatRect;
pub type Gfx_Color = libgfx_rust::Color;
pub type Compositing_VisualContextTree = Arc<VisualContextTree>;
pub type Compositing_ScrollStateSnapshot = crate::scrolling::ScrollStateSnapshot;
pub type Compositing_DisplayList = SharedDisplayList;
pub type Compositing_LayerPlan = LayerPlanOnWire;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
pub struct Web_DevicePixelPoint {
    pub x: i32,
    pub y: i32,
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

/// `Core::AnonymousBuffer`: shared memory, which goes as its size and a descriptor of it, or as nothing for a buffer
/// that holds nothing.
#[derive(Debug)]
pub struct Core_AnonymousBuffer {
    pub shared_memory: Option<(u64, File)>,
}

impl Encode for Core_AnonymousBuffer {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.shared_memory.is_some())?;
        if let Some((size, file)) = &self.shared_memory {
            encoder.encode(size)?;
            encoder.encode(file)?;
        }
        Ok(())
    }
}

impl Decode for Core_AnonymousBuffer {
    const MIN_ENCODED_SIZE: usize = size_of::<bool>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        if !decoder.decode::<bool>()? {
            return Ok(Self { shared_memory: None });
        }
        let size = decoder.decode()?;
        let file = decoder.decode()?;
        Ok(Self {
            shared_memory: Some((size, file)),
        })
    }
}

/// `Compositing::DisplayList`: a display list on its way to the compositor. It names its chunks in paint order, and
/// carries the records and runs of those the compositor does not keep packed in shared memory, which the compositor
/// copies out of it, with what travels beside them.
#[derive(Debug, Encode, Decode)]
pub struct SharedDisplayList {
    pub chunks_in_paint_order: Vec<ChunkId>,
    pub packed_chunks: Core_AnonymousBuffer,
    /// The size of each chunk packed, in the order they are packed.
    pub packed_chunk_sizes: Vec<PackedChunkSize>,
    pub properties: Compositing_DisplayListProperties,
    pub async_scrolling_metadata: Option<Compositing_AsyncScrollingMetadata>,
}

/// `Compositing::LayerPlan`: a plan of the layers of a display list on its way to the compositor. WebContent encodes a
/// plan it made, and the compositor decodes a plan that nothing reads until it passes the checks against the list and the
/// tree it is installed with.
#[derive(Debug)]
pub enum LayerPlanOnWire {
    Planned(Arc<LayerPlan>),
    Received(Box<UnvalidatedLayerPlan>),
}

impl LayerPlanOnWire {
    /// The plan a message from WebContent carried, which nothing reads until it is checked.
    pub fn into_received(self) -> UnvalidatedLayerPlan {
        match self {
            Self::Received(layer_plan) => *layer_plan,
            Self::Planned(_) => unreachable!("a message the compositor decoded carries a received plan"),
        }
    }
}

impl Encode for LayerPlanOnWire {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        match self {
            Self::Planned(plan) => encoder.encode(&**plan),
            Self::Received(_) => Err(EncodeError::InvalidValue(
                "A plan of layers goes on only from the process that planned it",
            )),
        }
    }
}

impl Decode for LayerPlanOnWire {
    const MIN_ENCODED_SIZE: usize = UnvalidatedLayerPlan::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self::Received(Box::new(decoder.decode()?)))
    }
}

impl Encode for ChunkId {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.0[0])?;
        encoder.encode(&self.0[1])
    }
}

impl Decode for ChunkId {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<u64>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self([decoder.decode()?, decoder.decode()?]))
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

include!(concat!(env!("OUT_DIR"), "/CompositorControlServerEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorPresentationClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorPresentationServerEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorWebContentClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/CompositorWebContentServerEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/RenderClockClientEndpoint.rs"));
include!(concat!(env!("OUT_DIR"), "/RenderClockServerEndpoint.rs"));
