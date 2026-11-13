/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text of a source, which is parked while nothing reads it: a large text that nothing asked for in a while is
//! compressed, and the next one to ask for it gets it decompressed.
//!
//! A source keeps its text for as long as it lives, to compile its functions when they are first called and to give
//! them their [[SourceText]]. Most functions of a page compile while it loads or never, so after that the text is
//! mostly dead weight, and for script-heavy pages it is the largest single thing the engine holds.
//!
//! Collections decide what to park: the sweep of a collection looks for texts that idled long enough and hands them to
//! a thread that compresses them. A text that something asked for in the meantime stays as it is.
//!
//! Nothing may keep a reference into the text of a source across the calls of [`SourceText::get()`]: what a caller
//! reads belongs to the string that the call gave it, and that string stays alive on its own.

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock, Weak};
use std::time::{Duration, Instant};

use ak::{Utf16String, Utf16StringUnits};
use libcompress_rust::{Compressor, Decompressor, Format};

/// The smallest text that is worth parking, in bytes.
const SMALLEST_PARKED_TEXT_SIZE: usize = 10 * 1024;

/// How long nothing has to ask for a text before it is parked.
const IDLE_TIME_BEFORE_PARKING: Duration = Duration::from_secs(20);

/// A parked text has to be at most this share of its size, or it stays as it is.
const LARGEST_PARKED_SHARE: (usize, usize) = (3, 4);

#[derive(Clone, Copy, PartialEq, Eq)]
enum Storage {
    Ascii,
    Latin1,
    Utf16,
}

struct ParkedText {
    /// The code units, each as its bytes in the order of this machine, compressed.
    compressed: Box<[u8]>,
    storage: Storage,
}

enum State {
    Present(Utf16String),
    Parked(ParkedText),
}

pub struct SourceText {
    state: Mutex<State>,
    length_in_code_units: usize,
    /// Whether the text holds its code units in 16 bits each, which stays the same while it is parked.
    is_wide: bool,
    /// How often something asked for the text.
    use_count: AtomicU64,
    /// When something last asked for the text, in milliseconds since the first text was made.
    last_use_time: AtomicU64,
    /// Whether the text turned out to be better off as it is, so that no collection tries to park it again.
    stays_present: AtomicBool,
}

/// A text that idled long enough, with what a thread needs to park it.
struct ParkingCandidate {
    source_text: Arc<SourceText>,
    text: Utf16String,
    use_count: u64,
}

#[derive(Default)]
struct Registry {
    /// The texts that can be parked.
    texts: Vec<Weak<SourceText>>,
    /// The texts that are present, by the identity of their strings, so that the holders of one string share one text.
    present_by_identity: HashMap<usize, Weak<SourceText>>,
}

fn registry() -> &'static Mutex<Registry> {
    static REGISTRY: OnceLock<Mutex<Registry>> = OnceLock::new();
    REGISTRY.get_or_init(Mutex::default)
}

fn milliseconds_now() -> u64 {
    static START: OnceLock<Instant> = OnceLock::new();
    u64::try_from(START.get_or_init(Instant::now).elapsed().as_millis()).unwrap_or(u64::MAX)
}

fn storage_of(text: &Utf16String) -> (Storage, usize) {
    match text.as_units() {
        Utf16StringUnits::Ascii(units) => (Storage::Ascii, units.len()),
        Utf16StringUnits::Latin1(units) => (Storage::Latin1, units.len()),
        Utf16StringUnits::Utf16(units) => (Storage::Utf16, size_of_val(units)),
    }
}

impl SourceText {
    /// The text of a source that holds `text`. Sources made of one large string share one text, so that parking it frees
    /// the string.
    pub fn of(text: &Utf16String) -> Arc<Self> {
        let (storage, size) = storage_of(text);
        let make = || {
            Arc::new(Self {
                state: Mutex::new(State::Present(text.clone())),
                length_in_code_units: if storage == Storage::Utf16 { size / 2 } else { size },
                is_wide: storage == Storage::Utf16,
                use_count: AtomicU64::new(0),
                last_use_time: AtomicU64::new(milliseconds_now()),
                stays_present: AtomicBool::new(false),
            })
        };
        if size < SMALLEST_PARKED_TEXT_SIZE {
            return make();
        }
        let identity = text.raw_identity();
        let mut registry = registry().lock().expect("nothing panics with the registry");
        if let Some(source_text) = registry.present_by_identity.get(&identity).and_then(Weak::upgrade) {
            source_text.note_use();
            return source_text;
        }
        let source_text = make();
        registry.texts.push(Arc::downgrade(&source_text));
        registry
            .present_by_identity
            .insert(identity, Arc::downgrade(&source_text));
        source_text
    }

    fn note_use(&self) {
        self.use_count.fetch_add(1, Ordering::Relaxed);
        self.last_use_time.store(milliseconds_now(), Ordering::Relaxed);
    }

    /// The text, which is decompressed first if it is parked.
    pub fn get(&self) -> Utf16String {
        self.note_use();
        let mut state = self.state.lock().expect("nothing panics with a text");
        match &*state {
            State::Present(text) => text.clone(),
            State::Parked(parked) => {
                let text = parked.unparked(self.length_in_code_units);
                *state = State::Present(text.clone());
                text
            }
        }
    }

    pub fn length_in_code_units(&self) -> usize {
        self.length_in_code_units
    }

    /// Whether the text holds its code units in 16 bits each.
    pub fn is_wide(&self) -> bool {
        self.is_wide
    }

    /// The bytes that the text takes up right now.
    pub fn memory_size(&self) -> usize {
        match &*self.state.lock().expect("nothing panics with a text") {
            State::Present(text) => storage_of(text).1,
            State::Parked(parked) => parked.compressed.len(),
        }
    }

    fn is_parked(&self) -> bool {
        matches!(
            &*self.state.lock().expect("nothing panics with a text"),
            State::Parked(_)
        )
    }
}

impl ParkedText {
    /// The parked form of `text`, or `None` if the text is better off as it is.
    fn of(text: &Utf16String) -> Option<Self> {
        let (storage, size) = storage_of(text);
        let mut compressor = Compressor::new(Format::Deflate);
        let mut compressed = match text.as_units() {
            Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => compressor.compress(units).ok()?,
            Utf16StringUnits::Utf16(units) => {
                // NB: A string of narrow code units comes back from Utf16String::from_utf16() with one byte for each,
                //     and the parser of a source goes by the width that the text had at first.
                if units.iter().all(|&unit| unit <= 0xFF) {
                    return None;
                }
                let mut compressed = Vec::new();
                for chunk in units.chunks(1 << 16) {
                    let bytes: Vec<u8> = chunk.iter().flat_map(|unit| unit.to_ne_bytes()).collect();
                    compressed.extend(compressor.compress(&bytes).ok()?);
                }
                compressed
            }
        };
        compressed.extend(compressor.finish().ok()?);
        if compressed.len() > size / LARGEST_PARKED_SHARE.1 * LARGEST_PARKED_SHARE.0 {
            return None;
        }
        Some(Self {
            compressed: compressed.into_boxed_slice(),
            storage,
        })
    }

    fn unparked(&self, length_in_code_units: usize) -> Utf16String {
        let bytes = Decompressor::new(Format::Deflate)
            .decompress(&self.compressed)
            .expect("a parked text is what the compressor made of it")
            .output;
        let text = match self.storage {
            Storage::Ascii => Utf16String::from_ascii(&bytes),
            Storage::Latin1 => Utf16String::from_latin1(&bytes),
            Storage::Utf16 => {
                let units: Vec<u16> = bytes
                    .as_chunks::<2>()
                    .0
                    .iter()
                    .map(|bytes| u16::from_ne_bytes(*bytes))
                    .collect();
                Utf16String::from_utf16(&units)
            }
        };
        assert_eq!(storage_of(&text).0 == Storage::Utf16, self.storage == Storage::Utf16);
        assert_eq!(
            match text.as_units() {
                Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.len(),
                Utf16StringUnits::Utf16(units) => units.len(),
            },
            length_in_code_units
        );
        text
    }
}

impl ParkingCandidate {
    /// Parks the text, unless something asked for it since it became a candidate.
    fn park(self) {
        let Some(parked) = ParkedText::of(&self.text) else {
            // NB: The text never changes, so compressing it again would come to the same.
            self.source_text.stays_present.store(true, Ordering::Relaxed);
            return;
        };
        let identity = self.text.raw_identity();
        let mut registry = registry().lock().expect("nothing panics with the registry");
        let mut state = self.source_text.state.lock().expect("nothing panics with a text");
        if self.source_text.use_count.load(Ordering::Relaxed) != self.use_count {
            return;
        }
        // NB: The string can be freed once it is parked, and its identity can then become that of another string.
        if registry
            .present_by_identity
            .get(&identity)
            .is_some_and(|present| core::ptr::eq(present.as_ptr(), Arc::as_ptr(&self.source_text)))
        {
            registry.present_by_identity.remove(&identity);
        }
        *state = State::Parked(parked);
    }
}

/// The texts that nothing asked for in `idle_time`.
fn parking_candidates(idle_time: Duration) -> Vec<ParkingCandidate> {
    let now = milliseconds_now();
    let idle_time = u64::try_from(idle_time.as_millis()).unwrap_or(u64::MAX);
    let mut candidates = Vec::new();
    let mut registry = registry().lock().expect("nothing panics with the registry");
    registry.texts.retain(|source_text| {
        let Some(source_text) = source_text.upgrade() else {
            return false;
        };
        if source_text.stays_present.load(Ordering::Relaxed) {
            return false;
        }
        let use_count = source_text.use_count.load(Ordering::Relaxed);
        if now.saturating_sub(source_text.last_use_time.load(Ordering::Relaxed)) < idle_time {
            return true;
        }
        // NB: Something that holds the text is reading it.
        let Ok(state) = source_text.state.try_lock() else {
            return true;
        };
        if let State::Present(text) = &*state {
            let text = text.clone();
            drop(state);
            candidates.push(ParkingCandidate {
                source_text,
                text,
                use_count,
            });
        }
        true
    });
    registry.present_by_identity.retain(|_, text| text.strong_count() > 0);
    candidates
}

/// Parks every text that can be parked, whatever its age, before it returns, and returns how many it parked. Tests call
/// this.
pub fn park_all_source_texts_now() -> usize {
    let candidates = parking_candidates(Duration::ZERO);
    let source_texts: Vec<_> = candidates
        .iter()
        .map(|candidate| candidate.source_text.clone())
        .collect();
    for candidate in candidates {
        candidate.park();
    }
    source_texts
        .iter()
        .filter(|source_text| source_text.is_parked())
        .count()
}

/// Parks the texts that nothing asked for in a while, on a thread of their own. Collections call this.
pub fn park_idle_source_texts() {
    static IS_PARKING: AtomicBool = AtomicBool::new(false);
    if IS_PARKING.swap(true, Ordering::Acquire) {
        return;
    }
    let candidates = parking_candidates(IDLE_TIME_BEFORE_PARKING);
    if candidates.is_empty() {
        IS_PARKING.store(false, Ordering::Release);
        return;
    }
    let park = move || {
        for candidate in candidates {
            candidate.park();
        }
        // NB: The allocator keeps what a thread freed until the thread allocates again, which this one never does.
        ak::release_unused_allocator_memory();
        IS_PARKING.store(false, Ordering::Release);
    };
    if std::thread::Builder::new()
        .name("LibJS source parking".into())
        .spawn(park)
        .is_err()
    {
        IS_PARKING.store(false, Ordering::Release);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Parking looks at every text there is, so the tests that park take turns.
    static TURN: Mutex<()> = Mutex::new(());

    fn large_text(with: &str) -> String {
        let mut text = String::new();
        while text.len() < 4 * SMALLEST_PARKED_TEXT_SIZE {
            text.push_str("function f() { return 'some text that repeats'; }\n");
        }
        text.push_str(with);
        text
    }

    #[test]
    fn a_parked_text_comes_back_as_it_was() {
        let _turn = TURN.lock();
        let latin1 = large_text("").bytes().chain([0xE9]).collect::<Vec<u8>>();
        for text in [
            Utf16String::from_utf8(&large_text("")),
            Utf16String::from_latin1(&latin1),
            Utf16String::from_utf8(&large_text("\u{2019}\u{1f600}")),
        ] {
            let source_text = SourceText::of(&text);
            let size = source_text.memory_size();
            park_all_source_texts_now();
            assert!(source_text.is_parked());
            assert!(source_text.memory_size() < size / 4);
            let unparked = source_text.get();
            assert!(!source_text.is_parked());
            assert!(unparked == text);
            assert_eq!(source_text.memory_size(), size);
            assert_eq!(
                source_text.is_wide(),
                matches!(unparked.as_units(), Utf16StringUnits::Utf16(_))
            );
        }
    }

    #[test]
    fn a_text_that_was_asked_for_meanwhile_stays_present() {
        let _turn = TURN.lock();
        let text = Utf16String::from_utf8(&large_text("meanwhile"));
        let source_text = SourceText::of(&text);
        let candidates = parking_candidates(Duration::ZERO);
        source_text.get();
        for candidate in candidates {
            candidate.park();
        }
        assert!(!source_text.is_parked());
    }

    #[test]
    fn holders_of_one_string_share_one_text() {
        let _turn = TURN.lock();
        let text = Utf16String::from_utf8(&large_text("shared"));
        let source_text = SourceText::of(&text);
        assert!(Arc::ptr_eq(&source_text, &SourceText::of(&text)));
        park_all_source_texts_now();
        assert!(source_text.is_parked());
        assert!(!Arc::ptr_eq(&source_text, &SourceText::of(&source_text.get())));
    }

    #[test]
    fn a_small_text_is_not_parked() {
        let _turn = TURN.lock();
        let source_text = SourceText::of(&Utf16String::from_utf8("let a = 1;"));
        park_all_source_texts_now();
        assert!(!source_text.is_parked());
    }
}
