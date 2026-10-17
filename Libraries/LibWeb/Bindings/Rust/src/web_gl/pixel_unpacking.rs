/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the pixels of a 2D texture image are in the client memory that GL unpacks it from, and what
//! UNPACK_FLIP_Y_WEBGL and UNPACK_PREMULTIPLY_ALPHA_WEBGL do to them, as arithmetic on plain bytes that the tests of
//! the crate cover without the JS runtime.

use core::ops::Range;

use super::constants::{
    GL_ALPHA, GL_BYTE, GL_DEPTH_COMPONENT, GL_FLOAT, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, GL_HALF_FLOAT, GL_INT,
    GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_RED, GL_RED_INTEGER, GL_RG, GL_RG_INTEGER, GL_RGB, GL_RGB_INTEGER, GL_RGBA,
    GL_RGBA_INTEGER, GL_SHORT, GL_UNSIGNED_BYTE, GL_UNSIGNED_INT, GL_UNSIGNED_INT_2_10_10_10_REV,
    GL_UNSIGNED_INT_5_9_9_9_REV, GL_UNSIGNED_INT_10F_11F_11F_REV, GL_UNSIGNED_INT_24_8, GL_UNSIGNED_SHORT,
    GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_SHORT_5_6_5,
};

/// Mirrors `Compositing::WebGL::PixelUnpackState`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct PixelUnpackState {
    pub(crate) alignment: usize,
    pub(crate) row_length: usize,
    pub(crate) skip_pixels: usize,
    pub(crate) skip_rows: usize,
}

impl Default for PixelUnpackState {
    fn default() -> Self {
        Self {
            alignment: 4,
            row_length: 0,
            skip_pixels: 0,
            skip_rows: 0,
        }
    }
}

/// https://registry.khronos.org/webgl/specs/latest/1.0/#PIXEL_STORAGE_PARAMETERS
///
/// A 2D image that texImage2D or texSubImage2D unpacks from an ArrayBufferView, and how it is flipped and
/// premultiplied on the way.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ImageUnpackConversion {
    pub(crate) width: i32,
    pub(crate) height: i32,
    pub(crate) format: u32,
    pub(crate) r#type: u32,
    pub(crate) unpack_state: PixelUnpackState,
    pub(crate) flip_y: bool,
    pub(crate) premultiply_alpha: bool,
}

impl ImageUnpackConversion {
    /// Converts the pixels where they are, so that GL unpacks the converted image with the same pixel unpack state.
    /// The bytes between the rows and before the first pixel stay as they are, and bytes that do not hold the whole
    /// image are left for GL to report the error of.
    pub(crate) fn convert(&self, bytes: &mut [u8]) {
        let Some(texel_layout) = texel_layout(self.format, self.r#type) else {
            return;
        };
        let Some(rows) = ImageRows::new(self, texel_layout.bytes_per_pixel) else {
            return;
        };
        if rows.end > bytes.len() {
            return;
        }

        if self.premultiply_alpha && texel_layout.alpha_premultiplication != AlphaPremultiplication::Unneeded {
            for row in 0..rows.count {
                premultiply_alpha_of_row(&mut bytes[rows.bytes_of(row)], texel_layout);
            }
        }
        if self.flip_y {
            flip_rows(bytes, &rows);
        }
    }
}

/// Where the rows of the pixels of an image are. A row is followed by the padding that UNPACK_ALIGNMENT asks for, and
/// by the pixels past the image when UNPACK_ROW_LENGTH is wider than the image.
#[derive(Debug, PartialEq, Eq)]
struct ImageRows {
    start_of_first_row: usize,
    stride: usize,
    byte_length: usize,
    count: usize,
    end: usize,
}

impl ImageRows {
    /// `None` for an image without pixels, and for one whose rows overlap.
    fn new(image: &ImageUnpackConversion, bytes_per_pixel: usize) -> Option<Self> {
        let width = usize::try_from(image.width).ok().filter(|&width| width > 0)?;
        let count = usize::try_from(image.height).ok().filter(|&height| height > 0)?;
        let unpack_state = &image.unpack_state;

        let pixels_per_row = if unpack_state.row_length == 0 {
            width
        } else {
            unpack_state.row_length
        };
        let stride = pixels_per_row
            .checked_mul(bytes_per_pixel)?
            .checked_next_multiple_of(unpack_state.alignment)?;
        let byte_length = width.checked_mul(bytes_per_pixel)?;
        if byte_length > stride {
            return None;
        }
        let start_of_first_row = unpack_state
            .skip_rows
            .checked_mul(stride)?
            .checked_add(unpack_state.skip_pixels.checked_mul(bytes_per_pixel)?)?;
        let end = stride
            .checked_mul(count - 1)?
            .checked_add(start_of_first_row)?
            .checked_add(byte_length)?;
        Some(Self {
            start_of_first_row,
            stride,
            byte_length,
            count,
            end,
        })
    }

    fn bytes_of(&self, row: usize) -> Range<usize> {
        let start = self.start_of_first_row + row * self.stride;
        start..start + self.byte_length
    }
}

fn flip_rows(bytes: &mut [u8], rows: &ImageRows) {
    for upper_row in 0..rows.count / 2 {
        let lower_row = rows.count - 1 - upper_row;
        let (bytes_up_to_lower_row, bytes_from_lower_row) = bytes.split_at_mut(rows.bytes_of(lower_row).start);
        bytes_up_to_lower_row[rows.bytes_of(upper_row)].swap_with_slice(&mut bytes_from_lower_row[..rows.byte_length]);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ComponentType {
    UnsignedByte,
    Byte,
    UnsignedShort,
    Short,
    UnsignedInt,
    Int,
    HalfFloat,
    Float,
}

impl ComponentType {
    fn size(self) -> usize {
        match self {
            Self::UnsignedByte | Self::Byte => 1,
            Self::UnsignedShort | Self::Short | Self::HalfFloat => 2,
            Self::UnsignedInt | Self::Int | Self::Float => 4,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AlphaPremultiplication {
    /// The texel has no alpha, or no color for alpha to be multiplied into.
    Unneeded,
    /// Each component is a number of the type, and alpha is the last of them.
    IntoComponentsBeforeAlpha(ComponentType),
    UnsignedShort4444,
    UnsignedShort5551,
    UnsignedInt2101010Rev,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct TexelLayout {
    bytes_per_pixel: usize,
    alpha_premultiplication: AlphaPremultiplication,
}

/// Formats and types that no upload takes together may have a layout: GL rejects the upload whatever its pixels are.
fn texel_layout(format: u32, r#type: u32) -> Option<TexelLayout> {
    let format_is_rgba = matches!(format, GL_RGBA | GL_RGBA_INTEGER);
    let packed_texel = |bytes_per_pixel, alpha_premultiplication_of_rgba| TexelLayout {
        bytes_per_pixel,
        alpha_premultiplication: if format_is_rgba {
            alpha_premultiplication_of_rgba
        } else {
            AlphaPremultiplication::Unneeded
        },
    };
    match r#type {
        GL_UNSIGNED_SHORT_4_4_4_4 => return Some(packed_texel(2, AlphaPremultiplication::UnsignedShort4444)),
        GL_UNSIGNED_SHORT_5_5_5_1 => return Some(packed_texel(2, AlphaPremultiplication::UnsignedShort5551)),
        GL_UNSIGNED_INT_2_10_10_10_REV => return Some(packed_texel(4, AlphaPremultiplication::UnsignedInt2101010Rev)),
        GL_UNSIGNED_SHORT_5_6_5 => return Some(packed_texel(2, AlphaPremultiplication::Unneeded)),
        GL_UNSIGNED_INT_10F_11F_11F_REV | GL_UNSIGNED_INT_5_9_9_9_REV | GL_UNSIGNED_INT_24_8 => {
            return Some(packed_texel(4, AlphaPremultiplication::Unneeded));
        }
        GL_FLOAT_32_UNSIGNED_INT_24_8_REV => return Some(packed_texel(8, AlphaPremultiplication::Unneeded)),
        _ => {}
    }

    let component_type = match r#type {
        GL_UNSIGNED_BYTE => ComponentType::UnsignedByte,
        GL_BYTE => ComponentType::Byte,
        GL_UNSIGNED_SHORT => ComponentType::UnsignedShort,
        GL_SHORT => ComponentType::Short,
        GL_UNSIGNED_INT => ComponentType::UnsignedInt,
        GL_INT => ComponentType::Int,
        GL_HALF_FLOAT => ComponentType::HalfFloat,
        GL_FLOAT => ComponentType::Float,
        _ => return None,
    };
    let (component_count, alpha_premultiplication) = match format {
        GL_ALPHA | GL_LUMINANCE | GL_RED | GL_RED_INTEGER | GL_DEPTH_COMPONENT => (1, AlphaPremultiplication::Unneeded),
        GL_RG | GL_RG_INTEGER => (2, AlphaPremultiplication::Unneeded),
        GL_RGB | GL_RGB_INTEGER => (3, AlphaPremultiplication::Unneeded),
        GL_LUMINANCE_ALPHA => (2, AlphaPremultiplication::IntoComponentsBeforeAlpha(component_type)),
        GL_RGBA | GL_RGBA_INTEGER => (4, AlphaPremultiplication::IntoComponentsBeforeAlpha(component_type)),
        _ => return None,
    };
    Some(TexelLayout {
        bytes_per_pixel: component_count * component_type.size(),
        alpha_premultiplication,
    })
}

fn premultiply_alpha_of_row(row: &mut [u8], texel_layout: TexelLayout) {
    let bytes_per_pixel = texel_layout.bytes_per_pixel;
    match texel_layout.alpha_premultiplication {
        AlphaPremultiplication::Unneeded => {}
        AlphaPremultiplication::IntoComponentsBeforeAlpha(component_type) => match component_type {
            ComponentType::UnsignedByte => premultiply_components_before_alpha::<1, u8>(row, bytes_per_pixel),
            ComponentType::Byte => premultiply_components_before_alpha::<1, i8>(row, bytes_per_pixel),
            ComponentType::UnsignedShort => premultiply_components_before_alpha::<2, u16>(row, bytes_per_pixel),
            ComponentType::Short => premultiply_components_before_alpha::<2, i16>(row, bytes_per_pixel),
            ComponentType::UnsignedInt => premultiply_components_before_alpha::<4, u32>(row, bytes_per_pixel),
            ComponentType::Int => premultiply_components_before_alpha::<4, i32>(row, bytes_per_pixel),
            ComponentType::HalfFloat => premultiply_components_before_alpha::<2, HalfFloat>(row, bytes_per_pixel),
            ComponentType::Float => premultiply_components_before_alpha::<4, f32>(row, bytes_per_pixel),
        },
        AlphaPremultiplication::UnsignedShort4444 => replace_numbers_of_row(row, premultiplied_unsigned_short_4444),
        AlphaPremultiplication::UnsignedShort5551 => replace_numbers_of_row(row, premultiplied_unsigned_short_5551),
        AlphaPremultiplication::UnsignedInt2101010Rev => {
            replace_numbers_of_row(row, premultiplied_unsigned_int_2_10_10_10_rev);
        }
    }
}

/// A number of `SIZE` bytes in the byte order of the platform, which is that of the typed arrays of script and that GL
/// unpacks.
trait NativeEndianNumber<const SIZE: usize>: Copy {
    fn from_bytes(bytes: [u8; SIZE]) -> Self;
    fn to_bytes(self) -> [u8; SIZE];
}

trait ColorComponent<const SIZE: usize>: NativeEndianNumber<SIZE> {
    fn multiplied_by_alpha(self, alpha: Self) -> Self;
}

macro_rules! native_endian_numbers {
    ($($number:ty),*) => {$(
        impl NativeEndianNumber<{ size_of::<$number>() }> for $number {
            fn from_bytes(bytes: [u8; size_of::<$number>()]) -> Self {
                Self::from_ne_bytes(bytes)
            }

            fn to_bytes(self) -> [u8; size_of::<$number>()] {
                self.to_ne_bytes()
            }
        }
    )*};
}

native_endian_numbers!(u8, i8, u16, i16, u32, i32, f32);

/// `color` and `alpha` count up to `maximum`, which stands for 1.
fn product_of_unsigned_normalized(color: u64, alpha: u64, maximum: u64) -> u64 {
    (color * alpha + maximum / 2) / maximum
}

/// `color` and `alpha` count from `-maximum` to `maximum`, which stand for -1 and 1. The one number below `-maximum`
/// stands for -1 as well.
fn product_of_signed_normalized(color: i64, alpha: i64, maximum: i64) -> i64 {
    let product = color.max(-maximum) * alpha.max(-maximum);
    let magnitude = (product.abs() + maximum / 2) / maximum;
    magnitude * product.signum()
}

macro_rules! unsigned_normalized_color_components {
    ($($number:ty),*) => {$(
        impl ColorComponent<{ size_of::<$number>() }> for $number {
            fn multiplied_by_alpha(self, alpha: Self) -> Self {
                let product = product_of_unsigned_normalized(self.into(), alpha.into(), Self::MAX.into());
                Self::try_from(product).expect("a product of two numbers up to 1 is up to 1")
            }
        }
    )*};
}

macro_rules! signed_normalized_color_components {
    ($($number:ty),*) => {$(
        impl ColorComponent<{ size_of::<$number>() }> for $number {
            fn multiplied_by_alpha(self, alpha: Self) -> Self {
                let product = product_of_signed_normalized(self.into(), alpha.into(), Self::MAX.into());
                Self::try_from(product).expect("a product of two numbers from -1 to 1 is from -1 to 1")
            }
        }
    )*};
}

// NB: An integer format has no range that stands for 1, so its alpha is premultiplied as though it were normalized.
unsigned_normalized_color_components!(u8, u16, u32);
signed_normalized_color_components!(i8, i16, i32);

impl ColorComponent<4> for f32 {
    fn multiplied_by_alpha(self, alpha: Self) -> Self {
        self * alpha
    }
}

const F32_SIGN_BIT: u32 = 0x8000_0000;
const F32_EXPONENT_BIAS: u32 = 127;
const F32_MANTISSA_BITS: u32 = 23;
const F32_INFINITY_BITS: u32 = 0x7f80_0000;

#[derive(Clone, Copy)]
struct HalfFloat(u16);

impl NativeEndianNumber<2> for HalfFloat {
    fn from_bytes(bytes: [u8; 2]) -> Self {
        Self(u16::from_ne_bytes(bytes))
    }

    fn to_bytes(self) -> [u8; 2] {
        self.0.to_ne_bytes()
    }
}

impl ColorComponent<2> for HalfFloat {
    fn multiplied_by_alpha(self, alpha: Self) -> Self {
        // NB: The product of two half floats is exact in an f32, so it is rounded once.
        Self::from_f32(self.to_f32() * alpha.to_f32())
    }
}

impl HalfFloat {
    const SIGN_BIT: u16 = 0x8000;
    const EXPONENT_BIAS: u32 = 15;
    const MANTISSA_BITS: u32 = 10;
    const MANTISSA_MASK: u16 = 0x3ff;
    const MAXIMUM_EXPONENT: u32 = 0x1f;
    const INFINITY: u16 = 0x7c00;
    const QUIET_NAN: u16 = 0x7e00;

    fn to_f32(self) -> f32 {
        const SUBNORMAL_STEP: f32 = 1.0 / 16_777_216.0;
        let sign = u32::from(self.0 & Self::SIGN_BIT) << 16;
        let exponent = u32::from(self.0 >> Self::MANTISSA_BITS) & Self::MAXIMUM_EXPONENT;
        let mantissa = self.0 & Self::MANTISSA_MASK;
        let mantissa_of_f32 = u32::from(mantissa) << (F32_MANTISSA_BITS - Self::MANTISSA_BITS);
        let magnitude = match exponent {
            0 => (f32::from(mantissa) * SUBNORMAL_STEP).to_bits(),
            Self::MAXIMUM_EXPONENT => F32_INFINITY_BITS | mantissa_of_f32,
            _ => ((exponent + F32_EXPONENT_BIAS - Self::EXPONENT_BIAS) << F32_MANTISSA_BITS) | mantissa_of_f32,
        };
        f32::from_bits(sign | magnitude)
    }

    /// Rounds to the nearest half float, and to the one with an even mantissa between two.
    fn from_f32(value: f32) -> Self {
        const SMALLEST_MAGNITUDE_ABOVE_THE_RANGE: u32 = (F32_EXPONENT_BIAS + 16) << F32_MANTISSA_BITS;
        const SMALLEST_NORMAL_MAGNITUDE: u32 = (F32_EXPONENT_BIAS + 1 - HalfFloat::EXPONENT_BIAS) << F32_MANTISSA_BITS;
        // NB: The step between the f32s from this one to twice it is that of the subnormal half floats, so the f32
        //     addition rounds a subnormal magnitude to one of them.
        const SUBNORMAL_ROUNDING_ADDEND: u32 = (F32_EXPONENT_BIAS - 1) << F32_MANTISSA_BITS;
        const DROPPED_MANTISSA_BITS: u32 = F32_MANTISSA_BITS - HalfFloat::MANTISSA_BITS;

        let bits = value.to_bits();
        let sign = ((bits >> 16) as u16) & Self::SIGN_BIT;
        let magnitude = bits & !F32_SIGN_BIT;
        let half_magnitude = if magnitude >= SMALLEST_MAGNITUDE_ABOVE_THE_RANGE {
            if magnitude > F32_INFINITY_BITS {
                Self::QUIET_NAN
            } else {
                Self::INFINITY
            }
        } else if magnitude < SMALLEST_NORMAL_MAGNITUDE {
            let sum = f32::from_bits(magnitude) + f32::from_bits(SUBNORMAL_ROUNDING_ADDEND);
            (sum.to_bits() - SUBNORMAL_ROUNDING_ADDEND) as u16
        } else {
            let mantissa_is_odd = (magnitude >> DROPPED_MANTISSA_BITS) & 1;
            let rebiased = magnitude - ((F32_EXPONENT_BIAS - Self::EXPONENT_BIAS) << F32_MANTISSA_BITS);
            // NB: A carry out of the mantissa increments the exponent, up to that of infinity.
            let rounded = rebiased + (1 << (DROPPED_MANTISSA_BITS - 1)) - 1 + mantissa_is_odd;
            (rounded >> DROPPED_MANTISSA_BITS) as u16
        };
        Self(sign | half_magnitude)
    }
}

fn premultiply_components_before_alpha<const SIZE: usize, C: ColorComponent<SIZE>>(
    row: &mut [u8],
    bytes_per_pixel: usize,
) {
    for pixel in row.chunks_exact_mut(bytes_per_pixel) {
        let Some((alpha, colors)) = pixel.as_chunks_mut::<SIZE>().0.split_last_mut() else {
            continue;
        };
        let alpha = C::from_bytes(*alpha);
        for color in colors {
            *color = C::from_bytes(*color).multiplied_by_alpha(alpha).to_bytes();
        }
    }
}

fn replace_numbers_of_row<const SIZE: usize, N: NativeEndianNumber<SIZE>>(
    row: &mut [u8],
    replacement: impl Fn(N) -> N,
) {
    for number in row.as_chunks_mut::<SIZE>().0 {
        *number = replacement(N::from_bytes(*number)).to_bytes();
    }
}

/// Red, green, blue and alpha from the most significant bits on.
fn premultiplied_unsigned_short_4444(texel: u16) -> u16 {
    let alpha = u64::from(texel & 0xf);
    let premultiplied = |shift: u32| {
        let color = u64::from((texel >> shift) & 0xf);
        (product_of_unsigned_normalized(color, alpha, 0xf) as u16) << shift
    };
    premultiplied(12) | premultiplied(8) | premultiplied(4) | (texel & 0xf)
}

/// Red, green and blue in five bits each and alpha in the least significant bit.
fn premultiplied_unsigned_short_5551(texel: u16) -> u16 {
    if texel & 1 == 0 { 0 } else { texel }
}

/// Red, green and blue in ten bits each and alpha in the two most significant bits, from the least significant bits on.
fn premultiplied_unsigned_int_2_10_10_10_rev(texel: u32) -> u32 {
    let alpha = u64::from(texel >> 30);
    let premultiplied = |shift: u32| {
        let color = u64::from((texel >> shift) & 0x3ff);
        (product_of_unsigned_normalized(color, alpha, 0b11) as u32) << shift
    };
    premultiplied(0) | premultiplied(10) | premultiplied(20) | (texel & 0xc000_0000)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::web_gl::constants::GL_DEPTH_STENCIL;

    fn image(width: i32, height: i32, format: u32, r#type: u32) -> ImageUnpackConversion {
        ImageUnpackConversion {
            width,
            height,
            format,
            r#type,
            unpack_state: PixelUnpackState {
                alignment: 1,
                ..PixelUnpackState::default()
            },
            flip_y: false,
            premultiply_alpha: false,
        }
    }

    fn flipped(image: ImageUnpackConversion) -> ImageUnpackConversion {
        ImageUnpackConversion { flip_y: true, ..image }
    }

    fn premultiplied(image: ImageUnpackConversion) -> ImageUnpackConversion {
        ImageUnpackConversion {
            premultiply_alpha: true,
            ..image
        }
    }

    fn with_unpack_state(image: ImageUnpackConversion, unpack_state: PixelUnpackState) -> ImageUnpackConversion {
        ImageUnpackConversion { unpack_state, ..image }
    }

    fn converted(image: ImageUnpackConversion, bytes: &[u8]) -> Vec<u8> {
        let mut bytes = bytes.to_vec();
        image.convert(&mut bytes);
        bytes
    }

    fn converted_numbers<const SIZE: usize, N: NativeEndianNumber<SIZE>>(
        image: ImageUnpackConversion,
        numbers: &[N],
    ) -> Vec<N> {
        let bytes: Vec<u8> = numbers.iter().flat_map(|number| number.to_bytes()).collect();
        let converted_bytes = converted(image, &bytes);
        converted_bytes
            .as_chunks::<SIZE>()
            .0
            .iter()
            .map(|bytes| N::from_bytes(*bytes))
            .collect()
    }

    #[test]
    fn the_rows_of_an_image_follow_the_pixel_unpack_state() {
        let rows = |width, height, bytes_per_pixel, unpack_state| {
            let image = with_unpack_state(image(width, height, GL_RGBA, GL_UNSIGNED_BYTE), unpack_state);
            ImageRows::new(&image, bytes_per_pixel)
        };
        let unpack_state = |alignment, row_length, skip_pixels, skip_rows| PixelUnpackState {
            alignment,
            row_length,
            skip_pixels,
            skip_rows,
        };

        assert_eq!(
            rows(3, 2, 3, unpack_state(4, 0, 0, 0)),
            Some(ImageRows {
                start_of_first_row: 0,
                stride: 12,
                byte_length: 9,
                count: 2,
                end: 21,
            })
        );
        assert_eq!(rows(3, 2, 3, unpack_state(1, 0, 0, 0)).unwrap().stride, 9);
        assert_eq!(rows(3, 2, 3, unpack_state(8, 0, 0, 0)).unwrap().stride, 16);
        assert_eq!(rows(3, 2, 4, unpack_state(8, 0, 0, 0)).unwrap().stride, 16);
        assert_eq!(
            rows(2, 3, 4, unpack_state(4, 5, 2, 1)),
            Some(ImageRows {
                start_of_first_row: 28,
                stride: 20,
                byte_length: 8,
                count: 3,
                end: 76,
            })
        );

        assert_eq!(rows(0, 2, 4, unpack_state(4, 0, 0, 0)), None);
        assert_eq!(rows(2, 0, 4, unpack_state(4, 0, 0, 0)), None);
        assert_eq!(rows(-1, 2, 4, unpack_state(4, 0, 0, 0)), None);
        assert_eq!(rows(3, 2, 4, unpack_state(4, 2, 0, 0)), None);
        assert_eq!(rows(2, 2, 4, unpack_state(0, 0, 0, 0)), None);
        assert_eq!(rows(2, 2, 4, unpack_state(4, 0, 0, usize::MAX)), None);
        assert_eq!(rows(2, 2, 4, unpack_state(4, 0, usize::MAX, 0)), None);
        assert_eq!(rows(i32::MAX, i32::MAX, 8, unpack_state(8, usize::MAX / 8, 0, 0)), None);
    }

    #[test]
    fn flipping_swaps_the_pixels_of_rows_and_keeps_padding_and_skipped_bytes() {
        let rgb = image(2, 3, GL_RGB, GL_UNSIGNED_BYTE);
        let aligned_to_four = with_unpack_state(rgb, PixelUnpackState::default());
        #[rustfmt::skip]
        let bytes = [
            1, 2, 3, 4, 5, 6, 0xa0, 0xa1,
            7, 8, 9, 10, 11, 12, 0xa2, 0xa3,
            13, 14, 15, 16, 17, 18,
        ];
        #[rustfmt::skip]
        let expected = [
            13, 14, 15, 16, 17, 18, 0xa0, 0xa1,
            7, 8, 9, 10, 11, 12, 0xa2, 0xa3,
            1, 2, 3, 4, 5, 6,
        ];
        assert_eq!(converted(flipped(aligned_to_four), &bytes), expected);
        assert_eq!(converted(premultiplied(aligned_to_four), &bytes), bytes);
        assert_eq!(converted(aligned_to_four, &bytes), bytes);

        let one_byte_pixels = image(1, 4, GL_ALPHA, GL_UNSIGNED_BYTE);
        assert_eq!(converted(flipped(one_byte_pixels), &[1, 2, 3, 4]), [4, 3, 2, 1]);
        assert_eq!(
            converted(flipped(image(4, 1, GL_ALPHA, GL_UNSIGNED_BYTE)), &[1, 2, 3, 4]),
            [1, 2, 3, 4]
        );
    }

    #[test]
    fn flipping_and_premultiplying_leave_the_pixels_around_a_sub_image_as_they_are() {
        let sub_image = with_unpack_state(
            image(2, 2, GL_RGBA, GL_UNSIGNED_BYTE),
            PixelUnpackState {
                alignment: 4,
                row_length: 3,
                skip_pixels: 1,
                skip_rows: 1,
            },
        );
        let outside = [9, 9, 9, 0];
        let transparent = [255, 255, 255, 0];
        let one_fifth_opaque = [200, 100, 50, 51];
        let opaque = [10, 20, 30, 255];
        let bytes = [
            outside,
            outside,
            outside,
            outside,
            one_fifth_opaque,
            transparent,
            outside,
            opaque,
            opaque,
        ]
        .concat();
        let expected = [
            outside,
            outside,
            outside,
            outside,
            opaque,
            opaque,
            outside,
            [40, 20, 10, 51],
            [0, 0, 0, 0],
        ]
        .concat();
        assert_eq!(converted(flipped(premultiplied(sub_image)), &bytes), expected);
    }

    #[test]
    fn bytes_that_do_not_hold_the_whole_image_are_left_as_they_are() {
        let rgba = flipped(premultiplied(image(1, 2, GL_RGBA, GL_UNSIGNED_BYTE)));
        let too_few_bytes = [255, 255, 255, 0, 1, 2, 3];
        assert_eq!(converted(rgba, &too_few_bytes), too_few_bytes);

        let more_bytes = [255, 255, 255, 0, 1, 2, 3, 255, 7];
        assert_eq!(converted(rgba, &more_bytes), [1, 2, 3, 255, 0, 0, 0, 0, 7]);

        let unknown_type = flipped(image(1, 2, GL_RGBA, 0x1234));
        assert_eq!(converted(unknown_type, &[1, 2]), [1, 2]);
        let unknown_format = flipped(image(1, 2, 0x1234, GL_UNSIGNED_BYTE));
        assert_eq!(converted(unknown_format, &[1, 2]), [1, 2]);
    }

    #[test]
    fn unsigned_bytes_are_premultiplied_where_the_format_has_alpha() {
        let premultiplied_pixel =
            |format, pixel: &[u8]| converted(premultiplied(image(1, 1, format, GL_UNSIGNED_BYTE)), pixel);
        assert_eq!(premultiplied_pixel(GL_RGBA, &[200, 100, 50, 51]), [40, 20, 10, 51]);
        assert_eq!(premultiplied_pixel(GL_RGBA, &[255, 255, 255, 0]), [0, 0, 0, 0]);
        assert_eq!(premultiplied_pixel(GL_RGBA, &[1, 128, 255, 255]), [1, 128, 255, 255]);
        assert_eq!(premultiplied_pixel(GL_RGBA, &[255, 1, 3, 128]), [128, 1, 2, 128]);
        assert_eq!(
            premultiplied_pixel(GL_RGBA_INTEGER, &[200, 100, 50, 51]),
            [40, 20, 10, 51]
        );
        assert_eq!(premultiplied_pixel(GL_LUMINANCE_ALPHA, &[200, 51]), [40, 51]);
        assert_eq!(premultiplied_pixel(GL_ALPHA, &[51]), [51]);
        assert_eq!(premultiplied_pixel(GL_RGB, &[200, 100, 51]), [200, 100, 51]);
        assert_eq!(premultiplied_pixel(GL_RG, &[200, 51]), [200, 51]);
        assert_eq!(premultiplied_pixel(GL_LUMINANCE, &[51]), [51]);
    }

    #[test]
    fn wider_and_signed_components_are_premultiplied_as_normalized_numbers() {
        let rgba = |r#type| premultiplied(image(1, 1, GL_RGBA, r#type));
        assert_eq!(
            converted_numbers::<2, u16>(rgba(GL_UNSIGNED_SHORT), &[65535, 1000, 3, 13107]),
            [13107, 200, 1, 13107]
        );
        assert_eq!(
            converted_numbers::<4, u32>(rgba(GL_UNSIGNED_INT), &[u32::MAX, 1000, 0, u32::MAX / 5]),
            [u32::MAX / 5, 200, 0, u32::MAX / 5]
        );
        assert_eq!(
            converted_numbers::<1, i8>(rgba(GL_BYTE), &[127, -127, -128, 64]),
            [64, -64, -64, 64]
        );
        assert_eq!(
            converted_numbers::<1, i8>(rgba(GL_BYTE), &[100, -100, 0, -127]),
            [-100, 100, 0, -127]
        );
        assert_eq!(
            converted_numbers::<2, i16>(rgba(GL_SHORT), &[32767, -32767, 1000, 6553]),
            [6553, -6553, 200, 6553]
        );
        assert_eq!(
            converted_numbers::<4, i32>(rgba(GL_INT), &[i32::MAX, i32::MIN, 1000, i32::MAX / 5]),
            [i32::MAX / 5, -(i32::MAX / 5), 200, i32::MAX / 5]
        );
        assert_eq!(
            converted_numbers::<2, u16>(
                premultiplied(image(1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT)),
                &[65535, 0, 0, 0]
            ),
            [0, 0, 0, 0]
        );
    }

    #[test]
    fn floats_are_premultiplied_by_multiplication() {
        assert_eq!(
            converted_numbers::<4, f32>(premultiplied(image(1, 1, GL_RGBA, GL_FLOAT)), &[1.0, 0.5, -2.0, 0.25]),
            [0.25, 0.125, -0.5, 0.25]
        );
        assert_eq!(
            converted_numbers::<4, f32>(premultiplied(image(1, 1, GL_LUMINANCE_ALPHA, GL_FLOAT)), &[0.75, 0.5]),
            [0.375, 0.5]
        );
        assert_eq!(
            converted_numbers::<4, f32>(premultiplied(image(1, 1, GL_RGB, GL_FLOAT)), &[0.75, 0.5, 0.25]),
            [0.75, 0.5, 0.25]
        );

        let one = 0x3c00;
        let half = 0x3800;
        let quarter = 0x3400;
        let three_quarters = 0x3a00;
        let three_sixteenths = 0x3200;
        let eighth = 0x3000;
        assert_eq!(
            converted_numbers::<2, u16>(
                premultiplied(image(1, 1, GL_RGBA, GL_HALF_FLOAT)),
                &[one, three_quarters, half, quarter]
            ),
            [quarter, three_sixteenths, eighth, quarter]
        );
        assert_eq!(
            converted_numbers::<2, u16>(
                premultiplied(image(1, 1, GL_LUMINANCE_ALPHA, GL_HALF_FLOAT)),
                &[one, half]
            ),
            [half, half]
        );
    }

    #[test]
    fn half_floats_convert_to_and_from_f32s() {
        let to_f32 = |bits| HalfFloat(bits).to_f32();
        assert_eq!(to_f32(0x0000).to_bits(), 0.0f32.to_bits());
        assert_eq!(to_f32(0x8000).to_bits(), (-0.0f32).to_bits());
        assert_eq!(to_f32(0x3c00), 1.0);
        assert_eq!(to_f32(0xc000), -2.0);
        assert_eq!(to_f32(0x7bff), 65504.0);
        assert_eq!(to_f32(0x0400), 1.0 / 16384.0);
        assert_eq!(to_f32(0x0001), 1.0 / 16_777_216.0);
        assert_eq!(to_f32(0x03ff), 1023.0 / 16_777_216.0);
        assert_eq!(to_f32(0x7c00), f32::INFINITY);
        assert_eq!(to_f32(0xfc00), f32::NEG_INFINITY);
        assert!(to_f32(0x7e00).is_nan());

        let from_f32 = |value| HalfFloat::from_f32(value).0;
        assert_eq!(from_f32(0.0), 0x0000);
        assert_eq!(from_f32(-0.0), 0x8000);
        assert_eq!(from_f32(1.0), 0x3c00);
        assert_eq!(from_f32(-2.0), 0xc000);
        assert_eq!(from_f32(65504.0), 0x7bff);
        assert_eq!(from_f32(65519.0), 0x7bff);
        assert_eq!(from_f32(65520.0), 0x7c00);
        assert_eq!(from_f32(1.0e10), 0x7c00);
        assert_eq!(from_f32(f32::INFINITY), 0x7c00);
        assert_eq!(from_f32(f32::NEG_INFINITY), 0xfc00);
        assert_eq!(from_f32(f32::NAN) & 0x7e00, 0x7e00);
        assert_eq!(from_f32(1.0 / 16384.0), 0x0400);
        assert_eq!(from_f32(1.0 / 16_777_216.0), 0x0001);
        assert_eq!(from_f32(1.0 / 33_554_432.0), 0x0000);
        assert_eq!(from_f32(3.0 / 33_554_432.0), 0x0002);
        assert_eq!(from_f32(1.0 / 1.0e10), 0x0000);
        // Between two half floats, the one with the even mantissa.
        assert_eq!(from_f32(1.0 + 1.0 / 2048.0), 0x3c00);
        assert_eq!(from_f32(1.0 + 3.0 / 2048.0), 0x3c02);
        assert_eq!(from_f32(1.0 + 1.0 / 2048.0 + 1.0 / 65536.0), 0x3c01);

        for bits in (0..=0xffff_u16).filter(|&bits| !HalfFloat(bits).to_f32().is_nan()) {
            assert_eq!(from_f32(to_f32(bits)), bits);
        }
    }

    #[test]
    fn packed_texels_are_premultiplied_by_their_alpha_bits() {
        assert_eq!(premultiplied_unsigned_short_4444(0xfa56), 0x6426);
        assert_eq!(premultiplied_unsigned_short_4444(0xfff0), 0x0000);
        assert_eq!(premultiplied_unsigned_short_4444(0x123f), 0x123f);
        assert_eq!(premultiplied_unsigned_short_4444(0xf008), 0x8008);

        assert_eq!(premultiplied_unsigned_short_5551(0xfc01), 0xfc01);
        assert_eq!(premultiplied_unsigned_short_5551(0xfffe), 0x0000);

        let texel = |red: u32, green: u32, blue: u32, alpha: u32| (alpha << 30) | (blue << 20) | (green << 10) | red;
        assert_eq!(
            premultiplied_unsigned_int_2_10_10_10_rev(texel(1023, 300, 0, 2)),
            texel(682, 200, 0, 2)
        );
        assert_eq!(
            premultiplied_unsigned_int_2_10_10_10_rev(texel(1023, 1023, 1023, 0)),
            texel(0, 0, 0, 0)
        );
        assert_eq!(
            premultiplied_unsigned_int_2_10_10_10_rev(texel(5, 6, 7, 3)),
            texel(5, 6, 7, 3)
        );
        assert_eq!(
            premultiplied_unsigned_int_2_10_10_10_rev(texel(1000, 2, 1, 1)),
            texel(333, 1, 0, 1)
        );

        let premultiplied_texels = |format, r#type, texels: &[u16]| {
            converted_numbers(premultiplied(image(texels.len() as i32, 1, format, r#type)), texels)
        };
        assert_eq!(
            premultiplied_texels(GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, &[0xfa56, 0xfff0]),
            [0x6426, 0x0000]
        );
        assert_eq!(
            premultiplied_texels(GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, &[0xfc01, 0xfffe]),
            [0xfc01, 0x0000]
        );
        assert_eq!(
            premultiplied_texels(GL_RGB, GL_UNSIGNED_SHORT_5_6_5, &[0xfffe, 0x0000]),
            [0xfffe, 0x0000]
        );
        assert_eq!(
            converted_numbers::<4, u32>(
                premultiplied(image(1, 1, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV)),
                &[texel(1023, 300, 0, 2)]
            ),
            [texel(682, 200, 0, 2)]
        );
        assert_eq!(
            converted_numbers::<4, u32>(
                premultiplied(image(1, 1, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV)),
                &[texel(1023, 300, 0, 2)]
            ),
            [texel(1023, 300, 0, 2)]
        );
    }

    #[test]
    fn every_texel_layout_takes_as_many_bytes_as_gl_unpacks() {
        let bytes_per_pixel = |format, r#type| texel_layout(format, r#type).map(|layout| layout.bytes_per_pixel);
        assert_eq!(bytes_per_pixel(GL_RGBA, GL_UNSIGNED_BYTE), Some(4));
        assert_eq!(bytes_per_pixel(GL_RGB, GL_UNSIGNED_BYTE), Some(3));
        assert_eq!(bytes_per_pixel(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE), Some(2));
        assert_eq!(bytes_per_pixel(GL_LUMINANCE, GL_UNSIGNED_BYTE), Some(1));
        assert_eq!(bytes_per_pixel(GL_ALPHA, GL_UNSIGNED_BYTE), Some(1));
        assert_eq!(bytes_per_pixel(GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4), Some(2));
        assert_eq!(bytes_per_pixel(GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1), Some(2));
        assert_eq!(bytes_per_pixel(GL_RGB, GL_UNSIGNED_SHORT_5_6_5), Some(2));
        assert_eq!(bytes_per_pixel(GL_RGBA, GL_FLOAT), Some(16));
        assert_eq!(bytes_per_pixel(GL_LUMINANCE_ALPHA, GL_HALF_FLOAT), Some(4));
        assert_eq!(bytes_per_pixel(GL_RGBA_INTEGER, GL_UNSIGNED_INT), Some(16));
        assert_eq!(bytes_per_pixel(GL_RGB_INTEGER, GL_SHORT), Some(6));
        assert_eq!(bytes_per_pixel(GL_RG, GL_HALF_FLOAT), Some(4));
        assert_eq!(bytes_per_pixel(GL_RG_INTEGER, GL_INT), Some(8));
        assert_eq!(bytes_per_pixel(GL_RED, GL_BYTE), Some(1));
        assert_eq!(bytes_per_pixel(GL_RED_INTEGER, GL_UNSIGNED_SHORT), Some(2));
        assert_eq!(bytes_per_pixel(GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV), Some(4));
        assert_eq!(bytes_per_pixel(GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV), Some(4));
        assert_eq!(bytes_per_pixel(GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV), Some(4));
        assert_eq!(bytes_per_pixel(GL_DEPTH_COMPONENT, GL_UNSIGNED_INT), Some(4));
        assert_eq!(bytes_per_pixel(GL_DEPTH_COMPONENT, GL_FLOAT), Some(4));
        assert_eq!(bytes_per_pixel(GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8), Some(4));
        assert_eq!(
            bytes_per_pixel(GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV),
            Some(8)
        );
        assert_eq!(bytes_per_pixel(GL_RGBA, 0x1234), None);
        assert_eq!(bytes_per_pixel(0x1234, GL_UNSIGNED_BYTE), None);
    }
}
