/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Codecs for the Gfx values carried by IPC.

use crate::filter::{ChannelSelector, TurbulenceType};
use crate::{
    Color, ColorFilterType, CompositingAndBlendingOperator, CornerRadii, CornerRadius, FloatMatrix4x4, FloatPoint,
    FloatRect, FloatSize, IntPoint, IntRect, IntSize, InterpolationColorSpace, MaskKind, ScalingMode, WindingRule,
};
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

impl Encode for FloatSize {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.width)?;
        encoder.encode(&self.height)
    }
}

impl Decode for FloatSize {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<f32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            width: decoder.decode()?,
            height: decoder.decode()?,
        })
    }
}

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

impl Encode for CornerRadius {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.horizontal_radius)?;
        encoder.encode(&self.vertical_radius)
    }
}

impl Decode for CornerRadius {
    const MIN_ENCODED_SIZE: usize = 2 * size_of::<i32>();

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            horizontal_radius: decoder.decode()?,
            vertical_radius: decoder.decode()?,
        })
    }
}

impl Encode for CornerRadii {
    #[inline]
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        encoder.encode(&self.top_left)?;
        encoder.encode(&self.top_right)?;
        encoder.encode(&self.bottom_right)?;
        encoder.encode(&self.bottom_left)
    }
}

impl Decode for CornerRadii {
    const MIN_ENCODED_SIZE: usize = 4 * CornerRadius::MIN_ENCODED_SIZE;

    #[inline]
    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        Ok(Self {
            top_left: decoder.decode()?,
            top_right: decoder.decode()?,
            bottom_right: decoder.decode()?,
            bottom_left: decoder.decode()?,
        })
    }
}

macro_rules! impl_codec_of_enum_going_as_i32 {
    ($type:ident, $type_name:literal, $from_i32:expr) => {
        impl Encode for $type {
            #[inline]
            fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
                encoder.encode(&(*self as i32))
            }
        }

        impl Decode for $type {
            const MIN_ENCODED_SIZE: usize = size_of::<i32>();

            #[inline]
            fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
                let from_i32: fn(i32) -> Option<Self> = $from_i32;
                from_i32(decoder.decode()?).ok_or(DecodeError::InvalidValue($type_name))
            }
        }
    };
}

impl_codec_of_enum_going_as_i32!(WindingRule, "Gfx::WindingRule", |value| match value {
    value if value == WindingRule::Nonzero as i32 => Some(WindingRule::Nonzero),
    value if value == WindingRule::EvenOdd as i32 => Some(WindingRule::EvenOdd),
    _ => None,
});
impl_codec_of_enum_going_as_i32!(MaskKind, "Gfx::MaskKind", |value| match value {
    value if value == MaskKind::Alpha as i32 => Some(MaskKind::Alpha),
    value if value == MaskKind::Luminance as i32 => Some(MaskKind::Luminance),
    _ => None,
});
impl_codec_of_enum_going_as_i32!(
    CompositingAndBlendingOperator,
    "Gfx::CompositingAndBlendingOperator",
    CompositingAndBlendingOperator::from_i32
);
impl_codec_of_enum_going_as_i32!(ColorFilterType, "Gfx::ColorFilterType", ColorFilterType::from_i32);
impl_codec_of_enum_going_as_i32!(ScalingMode, "Gfx::ScalingMode", ScalingMode::from_i32);
impl_codec_of_enum_going_as_i32!(
    InterpolationColorSpace,
    "Gfx::InterpolationColorSpace",
    InterpolationColorSpace::from_i32
);
impl_codec_of_enum_going_as_i32!(ChannelSelector, "Gfx::ChannelSelector", ChannelSelector::from_i32);
impl_codec_of_enum_going_as_i32!(TurbulenceType, "Gfx::TurbulenceType", TurbulenceType::from_i32);

#[cfg(test)]
mod tests {
    use super::*;

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
            Err(DecodeError::InvalidValue("Gfx::WindingRule"))
        );
        assert_eq!(
            decoded::<MaskKind>(&encoded(&-1i32)),
            Err(DecodeError::InvalidValue("Gfx::MaskKind"))
        );
        assert_eq!(
            decoded::<CompositingAndBlendingOperator>(&encoded(&0i32)),
            Err(DecodeError::InvalidValue("Gfx::CompositingAndBlendingOperator"))
        );
        assert_eq!(
            decoded::<ColorFilterType>(&encoded(&(ColorFilterType::Sepia as i32 + 1))),
            Err(DecodeError::InvalidValue("Gfx::ColorFilterType"))
        );
    }
}
