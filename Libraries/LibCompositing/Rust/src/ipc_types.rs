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
use crate::display_list::storage::{DisplayListStorage, packed_size, receive_packed};
use crate::visual_context::VisualContextTree;
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder, File};
use std::sync::Arc;

pub type Web_CompositorContextId = CompositorContextId;
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

/// `Compositing::DisplayList`: a display list on its way to the compositor, with its tape and run table packed in shared
/// memory, which the compositor copies out of it, and what travels beside them.
#[derive(Debug)]
pub struct SharedDisplayList {
    pub packed_tape: Core_AnonymousBuffer,
    pub tape_size: u64,
    pub run_count: u64,
    pub properties: Compositing_DisplayListProperties,
    pub async_scrolling_metadata: Option<Compositing_AsyncScrollingMetadata>,
}

impl Encode for SharedDisplayList {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.packed_tape)?;
        encoder.encode(&self.tape_size)?;
        encoder.encode(&self.run_count)?;
        encoder.encode(&self.properties)?;
        encoder.encode(&self.async_scrolling_metadata)
    }
}

impl Decode for SharedDisplayList {
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            packed_tape: decoder.decode()?,
            tape_size: decoder.decode()?,
            run_count: decoder.decode()?,
            properties: decoder.decode()?,
            async_scrolling_metadata: decoder.decode()?,
        })
    }
}

/// A display list that another one nests, with the tree it was recorded against.
#[derive(Clone)]
pub struct NestedDisplayListResource {
    pub properties: Compositing_DisplayListProperties,
    pub storage: Arc<DisplayListStorage>,
    pub tree: Arc<VisualContextTree>,
}

impl NestedDisplayListResource {
    /// Encodes the list as a resource transaction carries it: what travels alongside its tape, the tape and run table,
    /// and its tree.
    pub fn encode_into(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.properties)?;
        encoder.encode_size(self.storage.bytes().len())?;
        encoder.encode_size(self.storage.command_runs().len())?;
        encoder.append_bytes(&self.storage.packed());
        encoder.encode(&self.tree)
    }
}

impl std::fmt::Debug for NestedDisplayListResource {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("NestedDisplayListResource")
            .field("properties", &self.properties)
            .finish_non_exhaustive()
    }
}

/// `Compositing::DisplayListResourceTransaction` as it reaches the compositor's logic, which holds no fonts and no
/// image frames: those go ahead of it, in messages that the C++ of the compositor takes.
#[derive(Clone, Debug, Default)]
pub struct Compositing_DisplayListResourceTransaction {
    pub display_lists: Vec<NestedDisplayListResource>,
    pub font_ids_to_remove: Vec<u64>,
    pub image_frame_ids_to_remove: Vec<u64>,
    pub display_list_ids_to_remove: Vec<u64>,
}

impl Compositing_DisplayListResourceTransaction {
    pub fn removes_anything(&self) -> bool {
        !self.font_ids_to_remove.is_empty()
            || !self.image_frame_ids_to_remove.is_empty()
            || !self.display_list_ids_to_remove.is_empty()
    }
}

impl Encode for Compositing_DisplayListResourceTransaction {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        // No fonts, and no image frames.
        encoder.encode_size(0)?;
        encoder.encode_size(0)?;
        encoder.encode_size(self.display_lists.len())?;
        for display_list in &self.display_lists {
            display_list.encode_into(encoder)?;
        }
        encoder.encode(&self.font_ids_to_remove)?;
        encoder.encode(&self.image_frame_ids_to_remove)?;
        encoder.encode(&self.display_list_ids_to_remove)
    }
}

impl Decode for Compositing_DisplayListResourceTransaction {
    const MIN_ENCODED_SIZE: usize = 0;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        if decoder.decode_size()? != 0 || decoder.decode_size()? != 0 {
            return Err(DecodeError::InvalidValue(
                "a display list's fonts and image frames go ahead of it",
            ));
        }
        let display_list_count = decoder.decode_size()?;
        let mut display_lists = Vec::new();
        for _ in 0..display_list_count {
            let properties: Compositing_DisplayListProperties = decoder.decode()?;
            let tape_size = decoder.decode_size()?;
            let run_count = decoder.decode_size()?;
            let packed_size =
                packed_size(tape_size, run_count).ok_or(DecodeError::InvalidValue("Display list sizes overflow"))?;
            let packed = decoder.read_bytes(packed_size)?;
            let storage = receive_packed(packed, tape_size, run_count).map_err(DecodeError::InvalidValue)?;
            let tree: Arc<VisualContextTree> = decoder.decode()?;
            if properties.compatible_visual_context_tree_structural_epoch != tree.structural_epoch {
                return Err(DecodeError::InvalidValue(
                    "Display list resource does not match its visual context tree",
                ));
            }
            if !tree.display_list_references_only_live_nodes(storage.command_runs()) {
                return Err(DecodeError::InvalidValue(
                    "Display list resource references nodes its visual context tree does not hold",
                ));
            }
            display_lists.push(NestedDisplayListResource {
                properties,
                storage,
                tree,
            });
        }
        Ok(Self {
            display_lists,
            font_ids_to_remove: decoder.decode()?,
            image_frame_ids_to_remove: decoder.decode()?,
            display_list_ids_to_remove: decoder.decode()?,
        })
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
