/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub const GL_NO_ERROR: u32 = 0;
pub const GL_COLOR: u32 = 0x1800;
pub const GL_EXTENSIONS: u32 = 0x1F03;
pub const GL_REQUESTABLE_EXTENSIONS_ANGLE: u32 = 0x93A8;

pub const GL_FRAMEBUFFER: u32 = 0x8D40;
pub const GL_DRAW_FRAMEBUFFER: u32 = 0x8CA9;
pub const GL_FRAMEBUFFER_BINDING: u32 = 0x8CA6;
pub const GL_DRAW_FRAMEBUFFER_BINDING: u32 = 0x8CA6;
pub const GL_RENDERBUFFER: u32 = 0x8D41;
pub const GL_RENDERBUFFER_BINDING: u32 = 0x8CA7;

pub const GL_COLOR_CLEAR_VALUE: u32 = 0x0C22;
pub const GL_DEPTH_CLEAR_VALUE: u32 = 0x0B73;
pub const GL_STENCIL_CLEAR_VALUE: u32 = 0x0B91;
pub const GL_COLOR_BUFFER_BIT: u32 = 0x4000;
pub const GL_DEPTH_BUFFER_BIT: u32 = 0x0100;
pub const GL_STENCIL_BUFFER_BIT: u32 = 0x0400;

pub const GL_UNPACK_ALIGNMENT: u32 = 0x0CF5;
pub const GL_UNPACK_ROW_LENGTH: u32 = 0x0CF2;
pub const GL_UNPACK_SKIP_PIXELS: u32 = 0x0CF4;
pub const GL_UNPACK_SKIP_ROWS: u32 = 0x0CF3;
pub const GL_UNPACK_IMAGE_HEIGHT: u32 = 0x806E;
pub const GL_UNPACK_SKIP_IMAGES: u32 = 0x806D;
pub const GL_PIXEL_UNPACK_BUFFER: u32 = 0x88EC;
pub const GL_PIXEL_UNPACK_BUFFER_BINDING: u32 = 0x88EF;
pub const GL_MAP_READ_BIT: u32 = 0x0001;

pub const GL_ALPHA: u32 = 0x1906;
pub const GL_RGB: u32 = 0x1907;
pub const GL_RGBA: u32 = 0x1908;
pub const GL_LUMINANCE: u32 = 0x1909;
pub const GL_LUMINANCE_ALPHA: u32 = 0x190A;
pub const GL_SRGB_EXT: u32 = 0x8C40;
pub const GL_SRGB_ALPHA_EXT: u32 = 0x8C42;
pub const GL_R8: u32 = 0x8229;
pub const GL_RG8: u32 = 0x822B;
pub const GL_RGB8: u32 = 0x8051;
pub const GL_RGBA8: u32 = 0x8058;
pub const GL_SRGB8: u32 = 0x8C41;
pub const GL_SRGB8_ALPHA8: u32 = 0x8C43;
pub const GL_RGB565: u32 = 0x8D62;
pub const GL_RGBA4: u32 = 0x8056;
pub const GL_RGB5_A1: u32 = 0x8057;
pub const GL_RGB10_A2: u32 = 0x8059;
pub const GL_RGB9_E5: u32 = 0x8C3D;
pub const GL_R11F_G11F_B10F: u32 = 0x8C3A;
pub const GL_R16F: u32 = 0x822D;
pub const GL_RG16F: u32 = 0x822F;
pub const GL_RGB16F: u32 = 0x881B;
pub const GL_RGBA16F: u32 = 0x881A;
pub const GL_R32F: u32 = 0x822E;
pub const GL_RG32F: u32 = 0x8230;
pub const GL_RGB32F: u32 = 0x8815;
pub const GL_RGBA32F: u32 = 0x8814;
pub const GL_RED_INTEGER: u32 = 0x8D94;
pub const GL_RG_INTEGER: u32 = 0x8228;
pub const GL_RGB_INTEGER: u32 = 0x8D98;
pub const GL_RGBA_INTEGER: u32 = 0x8D99;
