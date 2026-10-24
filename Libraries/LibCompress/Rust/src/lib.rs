/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Streaming compression and decompression of brotli, zlib, raw deflate and gzip data, with flate2 on zlib-rs and
//! with brotli.

mod compressor;
mod decompressor;

pub use compressor::Compressor;
pub use decompressor::{Decompressed, DecompressionFailed, Decompressor};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    /// RFC 7932
    Brotli,
    /// RFC 1950
    Zlib,
    /// RFC 1951
    Deflate,
    /// RFC 1952
    Gzip,
}

#[cfg(test)]
mod tests;
