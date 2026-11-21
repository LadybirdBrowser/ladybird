/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Codecs for the Gfx values carried by IPC.

use crate::FloatMatrix4x4;
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};

impl Encode for FloatMatrix4x4 {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        for element in self.elements.as_flattened() {
            encoder.encode(element)?;
        }
        Ok(())
    }
}

impl Decode for FloatMatrix4x4 {
    const MIN_ENCODED_SIZE: usize = 16 * size_of::<f32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let mut elements = [[0.0f32; 4]; 4];
        for element in elements.as_flattened_mut() {
            *element = decoder.decode()?;
        }
        Ok(Self { elements })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        ColorFilterType, CompositingAndBlendingOperator, CornerRadii, CornerRadius, FloatSize, MaskKind, WindingRule,
    };

    fn encoded<T: Encode>(value: &T) -> Vec<u8> {
        let mut encoder = Encoder::new();
        encoder.encode(value).unwrap();
        encoder.finish().0
    }

    fn decoded<T: Decode>(bytes: &[u8]) -> Result<T, DecodeError> {
        Decoder::new(bytes, []).decode()
    }

    #[test]
    fn matrices_sizes_and_corner_radii_go_as_their_numbers() {
        let matrix = FloatMatrix4x4 {
            elements: [
                [1.0, 2.0, 3.0, 4.0],
                [5.0, 6.0, 7.0, 8.0],
                [9.0, 10.0, 11.0, 12.0],
                [13.0, 14.0, 15.0, 16.0],
            ],
        };
        let bytes = encoded(&matrix);
        let numbers: Vec<u8> = (1..=16).flat_map(|number| (number as f32).to_ne_bytes()).collect();
        assert_eq!(bytes, numbers);
        assert_eq!(decoded::<FloatMatrix4x4>(&bytes), Ok(matrix));
        assert_eq!(
            decoded::<FloatMatrix4x4>(&bytes[..bytes.len() - 1]),
            Err(DecodeError::UnexpectedEnd)
        );

        let size = FloatSize {
            width: 1.5,
            height: 2.5,
        };
        assert_eq!(decoded::<FloatSize>(&encoded(&size)), Ok(size));

        let radius = |horizontal_radius, vertical_radius| CornerRadius {
            horizontal_radius,
            vertical_radius,
        };
        let radii = CornerRadii {
            top_left: radius(1, 2),
            top_right: radius(3, 4),
            bottom_right: radius(5, 6),
            bottom_left: radius(7, 8),
        };
        let bytes = encoded(&radii);
        assert_eq!(bytes, encoded(&[1i32, 2, 3, 4, 5, 6, 7, 8])[4..]);
        assert_eq!(decoded::<CornerRadii>(&bytes), Ok(radii));
    }

    #[test]
    fn paint_enums_go_as_their_numbers_and_refuse_unknown_ones() {
        assert_eq!(encoded(&WindingRule::EvenOdd), encoded(&(WindingRule::EvenOdd as i32)));
        assert_eq!(
            decoded::<WindingRule>(&encoded(&WindingRule::EvenOdd)),
            Ok(WindingRule::EvenOdd)
        );
        assert_eq!(
            decoded::<MaskKind>(&encoded(&MaskKind::Luminance)),
            Ok(MaskKind::Luminance)
        );
        assert_eq!(
            decoded::<CompositingAndBlendingOperator>(&encoded(&CompositingAndBlendingOperator::PlusLighter)),
            Ok(CompositingAndBlendingOperator::PlusLighter)
        );
        assert_eq!(
            decoded::<ColorFilterType>(&encoded(&ColorFilterType::Sepia)),
            Ok(ColorFilterType::Sepia)
        );

        assert_eq!(
            decoded::<WindingRule>(&encoded(&2i32)),
            Err(DecodeError::InvalidValue("WindingRule"))
        );
        assert_eq!(
            decoded::<MaskKind>(&encoded(&-1i32)),
            Err(DecodeError::InvalidValue("MaskKind"))
        );
        assert_eq!(
            decoded::<CompositingAndBlendingOperator>(&encoded(&0i32)),
            Err(DecodeError::InvalidValue("CompositingAndBlendingOperator"))
        );
        assert_eq!(
            decoded::<ColorFilterType>(&encoded(&(ColorFilterType::Sepia as i32 + 1))),
            Err(DecodeError::InvalidValue("ColorFilterType"))
        );
    }
}
