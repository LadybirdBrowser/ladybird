/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::io::{self, Write};

use brotli::CompressorWriter;
use flate2::Compression;
use flate2::write::{DeflateEncoder, GzEncoder, ZlibEncoder};

use crate::Format;

/// The size of the buffer that the brotli encoder compresses into before it writes to its output.
const BROTLI_BUFFER_SIZE: usize = 4096;
/// The quality and the window size that brotli uses by default.
const BROTLI_QUALITY: u32 = 11;
const BROTLI_WINDOW_BITS: u32 = 22;

/// The encoder of a format, which writes what it compresses to a buffer that it keeps, until it finishes.
enum Encoder {
    Brotli(Box<CompressorWriter<Vec<u8>>>),
    Zlib(ZlibEncoder<Vec<u8>>),
    Deflate(DeflateEncoder<Vec<u8>>),
    Gzip(GzEncoder<Vec<u8>>),
    Finished,
}

/// Compresses data that arrives in pieces, giving what it has compressed after each one.
pub struct Compressor {
    encoder: Encoder,
}

impl Compressor {
    pub fn new(format: Format) -> Self {
        let encoder = match format {
            Format::Brotli => Encoder::Brotli(Box::new(CompressorWriter::new(
                Vec::new(),
                BROTLI_BUFFER_SIZE,
                BROTLI_QUALITY,
                BROTLI_WINDOW_BITS,
            ))),
            Format::Zlib => Encoder::Zlib(ZlibEncoder::new(Vec::new(), Compression::default())),
            Format::Deflate => Encoder::Deflate(DeflateEncoder::new(Vec::new(), Compression::default())),
            Format::Gzip => Encoder::Gzip(GzEncoder::new(Vec::new(), Compression::default())),
        };
        Self { encoder }
    }

    /// Compresses the next bytes of the data, returning what the encoder output, which can be nothing.
    ///
    /// # Panics
    ///
    /// If the compressor has finished.
    pub fn compress(&mut self, bytes: &[u8]) -> io::Result<Vec<u8>> {
        let output = match &mut self.encoder {
            Encoder::Brotli(encoder) => {
                encoder.write_all(bytes)?;
                encoder.get_mut()
            }
            Encoder::Zlib(encoder) => {
                encoder.write_all(bytes)?;
                encoder.get_mut()
            }
            Encoder::Deflate(encoder) => {
                encoder.write_all(bytes)?;
                encoder.get_mut()
            }
            Encoder::Gzip(encoder) => {
                encoder.write_all(bytes)?;
                encoder.get_mut()
            }
            Encoder::Finished => panic!("a compressor compresses nothing after it finishes"),
        };
        Ok(core::mem::take(output))
    }

    /// Finishes the compressed data, returning the rest of what the encoder output.
    ///
    /// # Panics
    ///
    /// If the compressor has finished.
    pub fn finish(&mut self) -> io::Result<Vec<u8>> {
        match core::mem::replace(&mut self.encoder, Encoder::Finished) {
            // NB: The brotli encoder finishes the compressed data when it gives up its output.
            Encoder::Brotli(encoder) => Ok(encoder.into_inner()),
            Encoder::Zlib(encoder) => encoder.finish(),
            Encoder::Deflate(encoder) => encoder.finish(),
            Encoder::Gzip(encoder) => encoder.finish(),
            Encoder::Finished => panic!("a compressor finishes once"),
        }
    }
}
