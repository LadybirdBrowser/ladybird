/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text a style sheet was parsed from. Only DevTools reads it, so a text of some size is kept
//! compressed: a thread of its own compresses each text soon after the parse, and a reader gets a
//! decompressed copy.

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::style_value::RetainedString;
use libcompress_rust::{Compressor, Decompressor, Format};
use std::sync::{Arc, Mutex};

/// The smallest text that is worth compressing, in bytes.
const SMALLEST_COMPRESSED_TEXT_SIZE: usize = 10 * 1024;

enum State {
    Plain(RetainedString),
    Compressed { deflated: Box<[u8]>, is_latin1: bool },
}

/// The text of a sheet. A text that is too small to compress costs the sheet no more than the text itself.
pub(crate) enum SheetSourceText {
    Plain(RetainedString),
    Compressible(Arc<CompressibleText>),
}

impl SheetSourceText {
    /// The text of a parse that is not of a whole sheet, which has none.
    pub(crate) fn empty() -> Self {
        Self::Plain(RetainedString::from_ascii(String::new()))
    }

    /// The text of a sheet, which is compressed soon if that is worth it.
    pub(crate) fn new(source: TokenizerInput<'_>) -> Self {
        let plain = RetainedString::from_text(source);
        let size = match plain.units() {
            TokenizerInput::Latin1(bytes) => bytes.len(),
            TokenizerInput::Utf16(units) => size_of_val(units),
        };
        if size < SMALLEST_COMPRESSED_TEXT_SIZE {
            return Self::Plain(plain);
        }
        let text = Arc::new(CompressibleText::plain(plain));
        // NB: The tests that count what a parse allocates must not count what the queue allocates now and then.
        #[cfg(not(test))]
        compress_soon(&text);
        Self::Compressible(text)
    }

    pub(crate) fn to_utf16_string(&self) -> ak::Utf16String {
        match self {
            Self::Plain(text) => utf16_string_of(text),
            Self::Compressible(text) => text.to_utf16_string(),
        }
    }
}

fn utf16_string_of(text: &RetainedString) -> ak::Utf16String {
    match text.units() {
        TokenizerInput::Latin1(bytes) => ak::Utf16String::from_latin1(bytes),
        TokenizerInput::Utf16(units) => ak::Utf16String::from_utf16(units),
    }
}

pub(crate) struct CompressibleText {
    state: Mutex<State>,
}

impl CompressibleText {
    fn plain(text: RetainedString) -> Self {
        Self {
            state: Mutex::new(State::Plain(text)),
        }
    }

    fn to_utf16_string(&self) -> ak::Utf16String {
        match &*self.state.lock().expect("no reader of a sheet's text panics") {
            State::Plain(text) => utf16_string_of(text),
            State::Compressed { deflated, is_latin1 } => {
                let bytes = Decompressor::new(Format::Deflate)
                    .decompress(deflated)
                    .expect("a compressed text is what the compressor made of it")
                    .output;
                if *is_latin1 {
                    return ak::Utf16String::from_latin1(&bytes);
                }
                let units: Vec<u16> = bytes
                    .as_chunks::<2>()
                    .0
                    .iter()
                    .map(|bytes| u16::from_ne_bytes(*bytes))
                    .collect();
                ak::Utf16String::from_utf16(&units)
            }
        }
    }

    fn compress(&self) {
        // NB: The lock is not held while the text is compressed, so a reader does not wait for that.
        let text = match &*self.state.lock().expect("no reader of a sheet's text panics") {
            State::Plain(text) => text.clone(),
            State::Compressed { .. } => return,
        };
        let mut compressor = Compressor::new(Format::Deflate);
        let mut deflated = Vec::new();
        let (is_latin1, size) = match text.units() {
            TokenizerInput::Latin1(bytes) => {
                let Ok(more) = compressor.compress(bytes) else {
                    return;
                };
                deflated = more;
                (true, bytes.len())
            }
            TokenizerInput::Utf16(units) => {
                let mut bytes = Vec::new();
                for chunk in units.chunks(1 << 16) {
                    bytes.clear();
                    bytes.extend(chunk.iter().flat_map(|unit| unit.to_ne_bytes()));
                    let Ok(more) = compressor.compress(&bytes) else {
                        return;
                    };
                    deflated.extend(more);
                }
                (false, size_of_val(units))
            }
        };
        let Ok(end) = compressor.finish() else {
            return;
        };
        deflated.extend(end);
        if deflated.len() >= size {
            return;
        }
        *self.state.lock().expect("no reader of a sheet's text panics") = State::Compressed {
            deflated: deflated.into_boxed_slice(),
            is_latin1,
        };
    }
}

/// Hands the text to the thread that compresses the texts, which starts when the first text comes.
#[cfg(not(test))]
fn compress_soon(text: &Arc<CompressibleText>) {
    use std::sync::mpsc::{Sender, channel};
    use std::sync::{OnceLock, Weak};

    static QUEUE: OnceLock<Option<Mutex<Sender<Weak<CompressibleText>>>>> = OnceLock::new();
    let queue = QUEUE.get_or_init(|| {
        let (sender, receiver) = channel::<Weak<CompressibleText>>();
        std::thread::Builder::new()
            .name("CSS source text".into())
            .spawn(move || {
                let mut next = receiver.recv();
                while let Ok(text) = next {
                    if let Some(text) = text.upgrade() {
                        text.compress();
                    }
                    next = receiver.try_recv().or_else(|_| {
                        // NB: The allocator keeps what a thread freed until the thread allocates again, which this
                        //     thread does not do until the next text comes, and it frees each text it compresses.
                        //     Standalone Rust tests do not link the C++ allocator.
                        #[cfg(feature = "allocator")]
                        ak::release_unused_allocator_memory();
                        receiver.recv()
                    });
                }
            })
            .ok()
            .map(|_| Mutex::new(sender))
    });
    if let Some(queue) = queue {
        let _ = queue
            .lock()
            .expect("sending a text does not panic")
            .send(Arc::downgrade(text));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn units_of(text: &CompressibleText) -> Vec<u16> {
        text.to_utf16_string().to_utf16().into_owned()
    }

    #[test]
    fn a_compressed_text_reads_as_the_text_it_was() {
        let ascii = "a { color: red; } /* a comment */\n".repeat(1000);
        let wide: Vec<u16> = "b::before { content: \"\u{2192} \u{1f600}\"; }\n"
            .repeat(1000)
            .encode_utf16()
            .collect();

        let text = CompressibleText::plain(RetainedString::from_text(TokenizerInput::Latin1(ascii.as_bytes())));
        text.compress();
        assert!(
            matches!(&*text.state.lock().unwrap(), State::Compressed { deflated, .. } if deflated.len() < ascii.len() / 4)
        );
        assert_eq!(units_of(&text), ascii.encode_utf16().collect::<Vec<_>>());

        let text = CompressibleText::plain(RetainedString::from_text(TokenizerInput::Utf16(&wide)));
        text.compress();
        assert!(matches!(
            &*text.state.lock().unwrap(),
            State::Compressed { is_latin1: false, .. }
        ));
        assert_eq!(units_of(&text), wide);
    }

    #[test]
    fn a_text_that_does_not_get_smaller_stays_as_it_is() {
        let text = CompressibleText::plain(RetainedString::from_text(TokenizerInput::Latin1(b"a{}")));
        text.compress();
        assert!(matches!(&*text.state.lock().unwrap(), State::Plain(_)));
        assert_eq!(units_of(&text), "a{}".encode_utf16().collect::<Vec<_>>());
    }
}
