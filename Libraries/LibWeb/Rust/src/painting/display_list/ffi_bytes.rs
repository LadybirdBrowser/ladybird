/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::{
    AffineTransform, CapStyle, Color, ColorFilterType, CompositingAndBlendingOperator, CornerClip, CornerRadii,
    CornerRadius, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, GradientInterpolationMethod,
    GradientInterpolationType, HueInterpolationMethod, IntPoint, IntRect, IntSize, InterpolationColorSpace, JoinStyle,
    LineStyle, MaskKind, Orientation, PolarColorSpace, RectangularColorSpace, ScalingMode, ShouldAntiAlias,
    WindingRule,
};

pub trait FfiBytes: Sized {
    fn write_ffi_bytes(&self, out: &mut [u8]);

    fn to_ffi_bytes(&self) -> Vec<u8> {
        let mut bytes = vec![0u8; std::mem::size_of::<Self>()];
        self.write_ffi_bytes(&mut bytes);
        bytes
    }
}

macro_rules! impl_ffi_bytes_for_primitive {
    ($($ty:ty),* $(,)?) => {
        $(impl FfiBytes for $ty {
            #[inline]
            fn write_ffi_bytes(&self, out: &mut [u8]) {
                out.copy_from_slice(&self.to_ne_bytes());
            }
        })*
    };
}
impl_ffi_bytes_for_primitive!(u8, i8, u16, i16, u32, i32, u64, i64, usize, isize, f32, f64);

impl FfiBytes for bool {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        out[0] = u8::from(*self);
    }
}

impl<T: FfiBytes, const N: usize> FfiBytes for [T; N] {
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        let element_size = std::mem::size_of::<T>();
        for (index, element) in self.iter().enumerate() {
            element.write_ffi_bytes(&mut out[index * element_size..(index + 1) * element_size]);
        }
    }
}

#[macro_export]
macro_rules! ffi_bytes_fields {
    ($name:ident { $($field:ident),* $(,)? }) => {
        impl $crate::painting::display_list::ffi_bytes::FfiBytes for $name {
            fn write_ffi_bytes(&self, out: &mut [u8]) {
                debug_assert_eq!(out.len(), ::std::mem::size_of::<Self>());
                $(
                    let offset = ::std::mem::offset_of!(Self, $field);
                    let size = ::std::mem::size_of_val(&self.$field);
                    self.$field.write_ffi_bytes(&mut out[offset..offset + size]);
                )*
            }
        }
    };
}

#[macro_export]
macro_rules! ffi_enum_bytes {
    ($name:ident as $repr:ty) => {
        impl $crate::painting::display_list::ffi_bytes::FfiBytes for $name {
            #[inline]
            fn write_ffi_bytes(&self, out: &mut [u8]) {
                (*self as $repr).write_ffi_bytes(out);
            }
        }
    };
}

ffi_bytes_fields!(IntPoint { x, y });
ffi_bytes_fields!(FloatPoint { x, y });
ffi_bytes_fields!(IntSize { width, height });
ffi_bytes_fields!(FloatSize { width, height });
ffi_bytes_fields!(IntRect { x, y, width, height });
ffi_bytes_fields!(FloatRect { x, y, width, height });
ffi_bytes_fields!(AffineTransform { values });
ffi_bytes_fields!(FloatMatrix4x4 { elements });
ffi_bytes_fields!(CornerRadius {
    horizontal_radius,
    vertical_radius
});
ffi_bytes_fields!(CornerRadii {
    top_left,
    top_right,
    bottom_right,
    bottom_left
});
ffi_bytes_fields!(GradientInterpolationMethod {
    interpolation_type,
    rectangular_color_space,
    polar_color_space,
    hue_interpolation_method
});

impl FfiBytes for Color {
    #[inline]
    fn write_ffi_bytes(&self, out: &mut [u8]) {
        self.0.write_ffi_bytes(out);
    }
}

ffi_enum_bytes!(WindingRule as i32);
ffi_enum_bytes!(LineStyle as u8);
ffi_enum_bytes!(ScalingMode as i32);
ffi_enum_bytes!(CompositingAndBlendingOperator as i32);
ffi_enum_bytes!(MaskKind as i32);
ffi_enum_bytes!(Orientation as i32);
ffi_enum_bytes!(ShouldAntiAlias as u8);
ffi_enum_bytes!(CornerClip as i32);
ffi_enum_bytes!(InterpolationColorSpace as i32);
ffi_enum_bytes!(RectangularColorSpace as u8);
ffi_enum_bytes!(PolarColorSpace as u8);
ffi_enum_bytes!(HueInterpolationMethod as u8);
ffi_enum_bytes!(GradientInterpolationType as u8);
ffi_enum_bytes!(CapStyle as i32);
ffi_enum_bytes!(JoinStyle as i32);
ffi_enum_bytes!(ColorFilterType as i32);
