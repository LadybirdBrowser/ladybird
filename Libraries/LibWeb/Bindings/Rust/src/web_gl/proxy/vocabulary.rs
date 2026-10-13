/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The vocabulary of the command stream: the command types, the synchronous call types and the struct that each
//! serializes to. The build script writes it from GLFunctions.json (generate/webgl_wire.rs).

include!(concat!(env!("OUT_DIR"), "/webgl_wire.rs"));

#[cfg(test)]
mod tests {
    use super::*;
    use crate::web_gl::proxy::wire::{DataSpan, WireStruct};

    #[test]
    fn a_struct_is_its_fields_in_the_byte_order_of_the_machine() {
        let command = commands::Viewport {
            x: 1,
            y: -2,
            width: 3,
            height: 4,
        };
        let mut expected = Vec::new();
        for field in [1i32, -2, 3, 4] {
            expected.extend_from_slice(&field.to_ne_bytes());
        }
        assert_eq!(command.as_bytes(), expected);
    }

    #[test]
    fn the_bytes_between_the_fields_of_a_struct_are_zero() {
        let command = commands::BufferData {
            target: u32::MAX,
            size: -1,
            has_data: true,
            data: DataSpan {
                offset: u32::MAX,
                size: u32::MAX,
            },
            usage: u32::MAX,
            ..commands::BufferData::ZEROED
        };
        let bytes = command.as_bytes();
        assert_eq!(bytes.len(), 32);
        assert_eq!(bytes[4..8], [0; 4]);
        assert_eq!(bytes[16], 1);
        assert_eq!(bytes[17..20], [0; 3]);
        assert!(bytes[20..32].iter().all(|&byte| byte == 0xFF));
        assert!(commands::BufferData::ZEROED.as_bytes().iter().all(|&byte| byte == 0));
    }

    #[test]
    fn a_struct_without_fields_is_one_byte() {
        assert_eq!(sync_calls::GetErrorRequest::ZEROED.as_bytes(), [0]);
        assert_eq!(commands::Flush::ZEROED.as_bytes(), [0]);
    }

    #[test]
    fn the_types_are_numbered_in_the_order_of_their_definition() {
        assert_eq!(CommandType::ActiveTexture as u16, 0);
        assert_eq!(CommandType::AttachShader as u16, 1);
        assert_eq!(SyncCallType::CheckFramebufferStatus as u16, 0);
        assert!((CommandType::Viewport as u16) < COMMAND_TYPE_COUNT);
        assert!((SyncCallType::GetError as u16) < SYNC_CALL_TYPE_COUNT);
    }

    #[test]
    fn the_table_of_layout_words_describes_every_struct() {
        let mut struct_count = 0;
        let mut words = WIRE_STRUCT_LAYOUTS;
        while let [_name_hash, size, alignment, field_count, rest @ ..] = words {
            assert!(*size >= 1 && size % alignment == 0);
            let mut end_of_previous_field = 0;
            let (fields, rest) = rest.split_at(*field_count as usize * 4);
            for [_name_hash, _type_hash, offset, field_size] in fields.as_chunks::<4>().0 {
                assert!(*offset >= end_of_previous_field && offset + field_size <= *size);
                end_of_previous_field = offset + field_size;
            }
            struct_count += 1;
            words = rest;
        }
        assert!(words.is_empty());
        assert_eq!(
            struct_count,
            usize::from(COMMAND_TYPE_COUNT) + 2 * usize::from(SYNC_CALL_TYPE_COUNT)
        );
    }
}
