/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The IPC endpoints that this crate speaks, as the IPC compiler generates them for Rust from the same definitions as
//! the C++ code at the other end, and the types of their messages that no IPC file declares. The generated code names
//! a C++ type `Namespace::Name` as `crate::ipc_types::Namespace_Name`.

#![allow(non_camel_case_types)]

use libcompositing_rust::css_pixels::CssPixels;
use libcompositing_rust::display_list::commands::{CompositorContextId, CompositorScrollNodeKind, UniqueNodeId};
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

include!(env!("LIBWEB_RENDER_CLOCK_SERVER_ENDPOINT"));
include!(env!("LIBWEB_RENDER_CLOCK_CLIENT_ENDPOINT"));

pub type Web_CompositorContextId = CompositorContextId;

/// `Web::AsyncScrollNodeStableID`: what a scroll node is, in a way that outlives the paint snapshot it was in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Web_AsyncScrollNodeStableID {
    pub node_id: UniqueNodeId,
    pub kind: CompositorScrollNodeKind,
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
    const MIN_ENCODED_SIZE: usize = 8 + 1 + 1;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            node_id: decoder.decode()?,
            kind: decoder.decode()?,
            pseudo_element_type: decoder.decode()?,
        })
    }
}

/// `Web::CSSPixelPoint`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Web_CSSPixelPoint {
    pub x: CssPixels,
    pub y: CssPixels,
}

impl Encode for Web_CSSPixelPoint {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.x)?;
        encoder.encode(&self.y)
    }
}

impl Decode for Web_CSSPixelPoint {
    const MIN_ENCODED_SIZE: usize = 4 + 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
        })
    }
}

/// `Web::CompositorScrollOffset`: where the Compositor has scrolled a scroll node to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Web_CompositorScrollOffset {
    pub scroll_node: Web_AsyncScrollNodeStableID,
    pub offset: Web_CSSPixelPoint,
}

impl Encode for Web_CompositorScrollOffset {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.scroll_node)?;
        encoder.encode(&self.offset)
    }
}

impl Decode for Web_CompositorScrollOffset {
    const MIN_ENCODED_SIZE: usize = Web_AsyncScrollNodeStableID::MIN_ENCODED_SIZE + Web_CSSPixelPoint::MIN_ENCODED_SIZE;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            scroll_node: decoder.decode()?,
            offset: decoder.decode()?,
        })
    }
}

/// `Web::DevicePixelPoint`, whose coordinates go as the ints `Web::DevicePixels` holds.
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
    const MIN_ENCODED_SIZE: usize = 4 + 4;

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            x: decoder.decode()?,
            y: decoder.decode()?,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libipc_rust::EndpointMessage;

    fn encode_message(message: impl EndpointMessage) -> Vec<u8> {
        let (bytes, attachments) = message.encode_message().unwrap();
        assert!(attachments.is_empty());
        bytes
    }

    fn header(magic: u32, id: i32) -> Vec<u8> {
        [magic.to_ne_bytes(), id.to_ne_bytes()].concat()
    }

    // NB: The layouts below are what the C++ codecs write: AsyncScrollNodeStableID.cpp, PixelUnits.cpp, and the codec
    //     PixelUnits.ipc generates for Web::DevicePixelPoint.

    #[test]
    fn a_clock_tick_request_goes_as_cpp_writes_it() {
        let bytes = encode_message(render_clock_server::messages::RequestClockTick {
            context_id: CompositorContextId(7),
            maximum_frames_per_second: 60.0,
        });
        let mut expected = header(render_clock_server::ENDPOINT_MAGIC, 1);
        expected.extend_from_slice(&7u64.to_ne_bytes());
        expected.extend_from_slice(&60.0f64.to_ne_bytes());
        assert_eq!(bytes, expected);
    }

    #[test]
    fn a_clock_tick_with_scroll_offsets_decodes_as_cpp_writes_it() {
        let mut bytes = header(render_clock_client::ENDPOINT_MAGIC, 1);
        bytes.extend_from_slice(&3u64.to_ne_bytes());
        bytes.extend_from_slice(&123_456_789i64.to_ne_bytes());
        bytes.extend_from_slice(&16.5f64.to_ne_bytes());
        bytes.extend_from_slice(&1u32.to_ne_bytes());
        bytes.extend_from_slice(&(-42i64).to_ne_bytes());
        bytes.extend_from_slice(&[2, 5]);
        bytes.extend_from_slice(&(10 * 64i32).to_ne_bytes());
        bytes.extend_from_slice(&(-3i32).to_ne_bytes());
        assert_eq!(bytes.len(), 8 + 8 + 8 + 8 + 4 + 18);

        let Ok(render_clock_client::Message::ClockTick(tick)) =
            render_clock_client::Message::decode(&bytes, Vec::new())
        else {
            panic!("a clock tick decodes");
        };
        assert_eq!(tick.context_id, CompositorContextId(3));
        assert_eq!(tick.frame_time_nanoseconds, 123_456_789);
        assert_eq!(tick.frame_interval_milliseconds, 16.5);
        assert_eq!(
            tick.scroll_offsets,
            [Web_CompositorScrollOffset {
                scroll_node: Web_AsyncScrollNodeStableID {
                    node_id: UniqueNodeId(-42),
                    kind: CompositorScrollNodeKind::PseudoElement,
                    pseudo_element_type: 5,
                },
                offset: Web_CSSPixelPoint {
                    x: CssPixels::from_raw(10 * 64),
                    y: CssPixels::from_raw(-3),
                },
            }]
        );

        // Re-encoding gives the same bytes.
        assert_eq!(encode_message(tick), bytes);
    }

    #[test]
    fn pointer_messages_decode_as_cpp_writes_them() {
        let mut moved = header(render_clock_client::ENDPOINT_MAGIC, 2);
        moved.extend_from_slice(&9u64.to_ne_bytes());
        moved.extend_from_slice(&(-5i32).to_ne_bytes());
        moved.extend_from_slice(&700i32.to_ne_bytes());
        moved.extend_from_slice(&1u32.to_ne_bytes());
        moved.push(1);
        moved.extend_from_slice(&77u64.to_ne_bytes());
        let Ok(render_clock_client::Message::PointerMoved(pointer)) =
            render_clock_client::Message::decode(&moved, Vec::new())
        else {
            panic!("a pointer move decodes");
        };
        assert_eq!(pointer.context_id, CompositorContextId(9));
        assert_eq!(pointer.position, Web_DevicePixelPoint { x: -5, y: 700 });
        assert_eq!(pointer.buttons, 1);
        assert!(pointer.scrolled_since_frame);
        assert_eq!(pointer.input_event_id, 77);

        let mut left = header(render_clock_client::ENDPOINT_MAGIC, 3);
        left.extend_from_slice(&9u64.to_ne_bytes());
        left.extend_from_slice(&78u64.to_ne_bytes());
        let Ok(render_clock_client::Message::PointerLeft(pointer)) =
            render_clock_client::Message::decode(&left, Vec::new())
        else {
            panic!("a pointer leaving decodes");
        };
        assert_eq!(pointer.context_id, CompositorContextId(9));
        assert_eq!(pointer.input_event_id, 78);
    }

    #[test]
    fn a_scroll_node_kind_cpp_does_not_have_is_refused() {
        let mut bytes = header(render_clock_client::ENDPOINT_MAGIC, 1);
        bytes.extend_from_slice(&[0; 24]);
        bytes.extend_from_slice(&1u32.to_ne_bytes());
        bytes.extend_from_slice(&[0; 8]);
        bytes.extend_from_slice(&[3, 0]);
        bytes.extend_from_slice(&[0; 8]);
        assert!(render_clock_client::Message::decode(&bytes, Vec::new()).is_err());
    }
}
