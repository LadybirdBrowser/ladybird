/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use brotli::{BrotliDecompressStream, BrotliResult, BrotliState, HeapAlloc, HuffmanCode};
use flate2::{Decompress, FlushDecompress, Status};

use crate::Format;

type BrotliDecoder = BrotliState<HeapAlloc<u8>, HeapAlloc<u32>, HeapAlloc<HuffmanCode>>;

/// The decoder of a format.
enum Decoder {
    Brotli(Box<BrotliDecoder>),
    // zlib, raw deflate and gzip, which zlib-rs decodes.
    Flate(Decompress),
}

/// How much the output of a decoder grows by while it decompresses.
const OUTPUT_GROWTH: usize = 16 * 1024;

/// The window that gzip data is decoded with, the largest that the format allows.
const GZIP_WINDOW_BITS: u8 = 15;

/// What decompressing some input gave: the output, and how many bytes at the end of the input the decoder did not
/// consume, which it leaves only once it has reached the end of the compressed data.
pub struct Decompressed {
    pub output: Vec<u8>,
    pub unconsumed_input: usize,
}

/// The input is not data of the format, with the decoder's description of why.
#[derive(Debug)]
pub struct DecompressionFailed(String);

impl fmt::Display for DecompressionFailed {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

/// Decompresses data that arrives in pieces, giving all that it can decompress of each one.
pub struct Decompressor {
    decoder: Decoder,
    has_reached_end: bool,
}

impl Decompressor {
    pub fn new(format: Format) -> Self {
        let decoder = match format {
            // NB: new_strict() decodes RFC 7932 brotli only, without its large window extension.
            Format::Brotli => Decoder::Brotli(Box::new(BrotliDecoder::new_strict(
                HeapAlloc::default(),
                HeapAlloc::default(),
                HeapAlloc::default(),
            ))),
            Format::Zlib => Decoder::Flate(Decompress::new(true)),
            Format::Deflate => Decoder::Flate(Decompress::new(false)),
            Format::Gzip => Decoder::Flate(Decompress::new_gzip(GZIP_WINDOW_BITS)),
        };
        Self {
            decoder,
            has_reached_end: false,
        }
    }

    /// Whether the decoder has reached the end of the compressed data.
    pub fn has_reached_end(&self) -> bool {
        self.has_reached_end
    }

    /// Decompresses the next bytes of the data, giving all the output that the decoder can make of them and of what it
    /// kept of the bytes before them. Bytes after the end of the compressed data are left unconsumed.
    pub fn decompress(&mut self, input: &[u8]) -> Result<Decompressed, DecompressionFailed> {
        let mut output = Vec::new();
        let unconsumed = if self.has_reached_end {
            input
        } else {
            match &mut self.decoder {
                Decoder::Brotli(decoder) => decompress_brotli(decoder, input, &mut output, &mut self.has_reached_end)?,
                Decoder::Flate(decoder) => decompress_flate(decoder, input, &mut output, &mut self.has_reached_end)?,
            }
        };
        Ok(Decompressed {
            output,
            unconsumed_input: unconsumed.len(),
        })
    }
}

/// Returns the input that the decoder did not consume.
fn decompress_brotli<'a>(
    decoder: &mut BrotliDecoder,
    input: &'a [u8],
    output: &mut Vec<u8>,
    has_reached_end: &mut bool,
) -> Result<&'a [u8], DecompressionFailed> {
    let mut available_in = input.len();
    let mut input_offset = 0;
    let mut total_out = 0;
    loop {
        let mut output_offset = output.len();
        output.resize(output_offset + OUTPUT_GROWTH, 0);
        let mut available_out = OUTPUT_GROWTH;
        let result = BrotliDecompressStream(
            &mut available_in,
            &mut input_offset,
            input,
            &mut available_out,
            &mut output_offset,
            output,
            &mut total_out,
            decoder,
        );
        output.truncate(output_offset);
        match result {
            BrotliResult::NeedsMoreOutput => {}
            BrotliResult::NeedsMoreInput => return Ok(&input[input_offset..]),
            BrotliResult::ResultSuccess => {
                *has_reached_end = true;
                return Ok(&input[input_offset..]);
            }
            BrotliResult::ResultFailure => return Err(DecompressionFailed(format!("{:?}", decoder.error_code))),
        }
    }
}

/// Returns the input that the decoder did not consume.
fn decompress_flate<'a>(
    decoder: &mut Decompress,
    mut input: &'a [u8],
    output: &mut Vec<u8>,
    has_reached_end: &mut bool,
) -> Result<&'a [u8], DecompressionFailed> {
    loop {
        output.reserve(OUTPUT_GROWTH);
        let total_in = decoder.total_in();
        let status = decoder
            .decompress_vec(input, output, FlushDecompress::None)
            .map_err(|error| DecompressionFailed(error.to_string()))?;
        let consumed = usize::try_from(decoder.total_in() - total_in).expect("the decoder consumed some of the input");
        input = &input[consumed..];
        match status {
            Status::StreamEnd => {
                *has_reached_end = true;
                return Ok(input);
            }
            // NB: The decoder stops when it runs out of input or of room for its output, and fails to make progress
            //     with BufError, so it has given all it can of this input once it stops short of both.
            Status::Ok if !input.is_empty() || output.len() == output.capacity() => {}
            Status::Ok | Status::BufError => return Ok(input),
        }
    }
}
