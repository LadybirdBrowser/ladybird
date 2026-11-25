/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The pixel formats that an image is converted to for a texture upload of a format and a type.

const GL_ALPHA: u32 = 0x1906;
const GL_RGB: u32 = 0x1907;
const GL_RGBA: u32 = 0x1908;
const GL_LUMINANCE: u32 = 0x1909;
const GL_LUMINANCE_ALPHA: u32 = 0x190A;
const GL_FLOAT: u32 = 0x1406;
const GL_UNSIGNED_BYTE: u32 = 0x1401;
const GL_UNSIGNED_SHORT_4_4_4_4: u32 = 0x8033;
const GL_UNSIGNED_SHORT_5_5_5_1: u32 = 0x8034;
const GL_UNSIGNED_SHORT_5_6_5: u32 = 0x8363;

/// `Gfx::ExportFormat`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ExportFormat {
    Luminance8,
    LuminanceAlpha88,
    Alpha8,
    RGB565,
    RGBA5551,
    RGBA4444,
    RGB888,
    RGBA8888,
    Luminance32F,
    LuminanceAlpha32F,
    Alpha32F,
    RGB32F,
    RGBA32F,
}

pub fn texture_export_format(format: u32, r#type: u32) -> Option<ExportFormat> {
    Some(match (format, r#type) {
        (GL_RGB, GL_UNSIGNED_BYTE) => ExportFormat::RGB888,
        (GL_RGB, GL_UNSIGNED_SHORT_5_6_5) => ExportFormat::RGB565,
        (GL_RGBA, GL_UNSIGNED_BYTE) => ExportFormat::RGBA8888,
        // FIXME: This is not exactly the same as RGBA.
        (GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4) => ExportFormat::RGBA4444,
        (GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1) => ExportFormat::RGBA5551,
        (GL_ALPHA, GL_UNSIGNED_BYTE) => ExportFormat::Alpha8,
        (GL_LUMINANCE, GL_UNSIGNED_BYTE) => ExportFormat::Luminance8,
        (GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE) => ExportFormat::LuminanceAlpha88,
        (GL_LUMINANCE, GL_FLOAT) => ExportFormat::Luminance32F,
        (GL_LUMINANCE_ALPHA, GL_FLOAT) => ExportFormat::LuminanceAlpha32F,
        (GL_ALPHA, GL_FLOAT) => ExportFormat::Alpha32F,
        (GL_RGB, GL_FLOAT) => ExportFormat::RGB32F,
        (GL_RGBA, GL_FLOAT) => ExportFormat::RGBA32F,
        _ => return None,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_the_formats_and_types_that_an_image_converts_to_have_an_export_format() {
        assert_eq!(
            texture_export_format(GL_RGBA, GL_UNSIGNED_BYTE),
            Some(ExportFormat::RGBA8888)
        );
        assert_eq!(
            texture_export_format(GL_RGB, GL_UNSIGNED_SHORT_5_6_5),
            Some(ExportFormat::RGB565)
        );
        assert_eq!(
            texture_export_format(GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE),
            Some(ExportFormat::LuminanceAlpha88)
        );
        assert_eq!(texture_export_format(GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4), None);
        assert_eq!(texture_export_format(GL_ALPHA, GL_UNSIGNED_SHORT_5_6_5), None);
    }
}
