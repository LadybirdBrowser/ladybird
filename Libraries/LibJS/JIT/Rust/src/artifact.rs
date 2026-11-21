/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tables of compiled code: what the runtime reads to install the code, to enter it, and to translate its state
//! into interpreter frames at its sites (see `code::SiteKind`).
//!
//! The tables are the sections of one array of 32-bit words, whose bounds a `Layout` holds. A record is a fixed
//! number of words, and records refer to each other and to runs of words by index. So the array holds no pointer: a
//! copy of it is the same tables, and the runtime reads them where they are, through the views of this module.
//!
//! A frame state of the graph becomes one frame record, however many sites have it and however many frame states
//! have it as their parent. What the frame states of one inlined call have in common is in a call record that they
//! share, and a frame of the compiled function itself needs none. What two sites with the same frame state differ in
//! is where the values are, so each site has a run of locations: those of the values of its frames, innermost frame
//! first and in slot order within a frame, then those of the properties of its objects.
//!
//! Code has the same constants, the same lists of slots and the same locations of values over and over, so equal
//! runs of words in those tables are stored one time (see `Shared`).

use core::cell::Cell;
use core::ops::Range;

use ak::Column;
use ak::Zone;
use ak::ZoneVec;

use crate::CompileFailure;
use crate::asm::JumpPatch;
use crate::code::CALLEE_SLOT;
use crate::code::Dependency;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::code::ResumeMode;
use crate::code::SiteKind;
use crate::code::ValueLocation;
use crate::code::VirtualObjectDescriptorKind;
use crate::snapshot::CellId;

/// A table of compiled code. Their order here is their order in the array.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Section {
    /// A record per site, by site index.
    Sites,
    /// A record per frame state that a site has, directly or as the caller of another.
    Frames,
    /// The call records that frame records refer to.
    Calls,
    /// The slots of the values of the frames, a sorted run for each list of slots.
    Slots,
    /// The slots frames hold already (see `FrameView::in_frame()`), a sorted run for each list of slots.
    InFrame,
    /// A record per object that the code never allocated, a run per frame that is the innermost one of a site.
    Objects,
    /// Where the values are, a run for the sites that have their values in the same places.
    Locations,
    /// The 64-bit values that locations refer to, as two words each, low half first.
    Wide,
    /// The pcs of the on-stack replacement entries, sorted.
    OsrPcs,
    /// The code offset of each on-stack replacement entry, in the order of their pcs.
    OsrOffsets,
    /// See `CompiledCode::osr_loop_back_edges()`. Sorted.
    OsrLoopBackEdges,
    /// See `CompiledCode::osr_outer_loop_back_edges()`. Sorted.
    OsrOuterLoopBackEdges,
    /// See `ir::Graph::uncaught_handlers`. Sorted.
    UncaughtHandlers,
    /// A record per invalidation patch.
    Patches,
    /// The cells the code embeds, as two words each. Only the install reads this section and the next.
    EmbeddedCells,
    /// A record per dependency.
    Dependencies,
}

const SECTION_COUNT: usize = Section::Dependencies as usize + 1;

/// The sections the runtime reads after it installed the code are the first ones.
const LAST_KEPT_SECTION: Section = Section::Patches;

/// In place of an index, a slot or a count: there is none.
const NONE: u32 = u32::MAX;

const SITE_WORDS: usize = 4;
/// The kind of the site in the low byte, and the kind of the exit of an exit site in the next one.
const SITE_KIND: usize = 0;
/// The record of the innermost frame.
const SITE_FRAME: usize = 1;
/// Where the run of locations of the site starts.
const SITE_LOCATIONS: usize = 2;
const SITE_INLINED_FRAME_BYTES: usize = 3;

const SITE_KIND_EXIT: u32 = 0;
const SITE_KIND_LEAVE: u32 = 1;
const SITE_KIND_PUBLISH: u32 = 2;
const SITE_KIND_CALL: u32 = 3;

const FRAME_WORDS: usize = 7;
const FRAME_PC: usize = 0;
/// The destination of a frame that resumes after its instruction.
const FRAME_RESUME_DESTINATION: usize = 1;
const FRAME_VALUE_COUNT: usize = 2;
const FRAME_SLOTS: usize = 3;
const FRAME_IN_FRAME: usize = 4;
const FRAME_IN_FRAME_COUNT: usize = 5;
/// Where the call record of the frame starts, or `NO_CALL`, above two bits.
const FRAME_LINK: usize = 6;

/// The bit of a link that says that the frame resumes after its instruction.
const LINK_RESUMES_AFTER: u32 = 1 << 0;
/// The bit of a link that says that the call record has the objects of the sites the frame is the innermost one of.
const LINK_HAS_OBJECTS: u32 = 1 << 1;
const LINK_CALL_SHIFT: u32 = 2;
/// In place of where a call record starts: the frame is one of the compiled function with nothing more to say.
const NO_CALL: u32 = u32::MAX >> LINK_CALL_SHIFT;

const CALL_WORDS: usize = 6;
const CALL_EXECUTABLE: usize = 0;
/// The record of the frame of the caller, or `NONE` for a frame of the compiled function.
const CALL_PARENT: usize = 1;
/// How many callers the frame has.
const CALL_DEPTH: usize = 2;
const CALL_PASSED_ARGUMENT_COUNT: usize = 3;
const CALL_LEGACY_ARGUMENTS_START: usize = 4;
const CALL_LEGACY_ARGUMENTS_END: usize = 5;
/// Only in a call record with objects: where they start, and how many there are.
const CALL_OBJECTS: usize = 6;
const CALL_OBJECT_COUNT: usize = 7;
const CALL_WORDS_WITH_OBJECTS: usize = 8;

const OBJECT_WORDS: usize = 6;
const OBJECT_KIND: usize = 0;
const OBJECT_ALLOCATION: usize = 1;
/// The shape or the sample slot, as two words.
const OBJECT_CELL: usize = 2;
/// Where the locations of the properties start in the run of a site.
const OBJECT_PROPERTIES: usize = 4;
const OBJECT_PROPERTY_COUNT: usize = 5;

const OBJECT_KIND_OBJECT: u32 = 0;
const OBJECT_KIND_CLOSURE: u32 = 1;

const WIDE_WORDS: usize = 2;

const PATCH_WORDS: usize = 4;
const PATCH_OFFSET: usize = 0;
const PATCH_LENGTH: usize = 1;
const PATCH_BYTES: usize = 2;

/// The most bytes an invalidation patch has.
pub const MAX_PATCH_LENGTH: usize = 8;

const CELL_WORDS: usize = 2;

const DEPENDENCY_WORDS: usize = 5;

// A location is one word: its kind in the low three bits, the representation of the value in the next three, and a
// payload in the rest.
const LOCATION_KIND_MASK: u32 = 0b111;
const LOCATION_REPR_SHIFT: u32 = 3;
const LOCATION_REPR_MASK: u32 = 0b111;
const LOCATION_PAYLOAD_SHIFT: u32 = 6;
const LOCATION_PAYLOAD_LIMIT: u32 = 1 << (u32::BITS - LOCATION_PAYLOAD_SHIFT);

/// The payload is the register.
const LOCATION_REGISTER: u32 = 0;
/// The payload is the offset, which has a sign.
const LOCATION_STACK: u32 = 1;
/// The payload is the slot.
const LOCATION_FRAME: u32 = 2;
/// The payload is the index of the constant among the 64-bit values.
const LOCATION_CONSTANT: u32 = 3;
/// The payload is whether the object is mapped.
const LOCATION_ARGUMENTS_OBJECT: u32 = 4;
/// The payload is the index of the object among those of the site.
const LOCATION_VIRTUAL_OBJECT: u32 = 5;

/// Where each table of compiled code is in its array of words.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Layout {
    /// Where each section ends, which is where the next one starts.
    ends: [u32; SECTION_COUNT],
}

impl Layout {
    fn range(&self, section: Section) -> Range<usize> {
        let index = section as usize;
        let start = if index == 0 { 0 } else { self.ends[index - 1] };
        start as usize..self.ends[index] as usize
    }

    /// How many words the tables have that the runtime reads after it installed the code. They are the first ones.
    pub fn kept_len(&self) -> usize {
        self.ends[LAST_KEPT_SECTION as usize] as usize
    }
}

/// A word of the tables, wherever they are.
pub trait Word {
    fn word(&self) -> u32;
}

impl Word for u32 {
    fn word(&self) -> u32 {
        *self
    }
}

/// A word of tables in memory that code with other tables there shares.
impl Word for Cell<u32> {
    fn word(&self) -> u32 {
        self.get()
    }
}

fn halves(value: u64) -> [u32; 2] {
    [value as u32, (value >> 32) as u32]
}

fn from_halves<W: Word>(words: &[W]) -> u64 {
    u64::from(words[0].word()) | u64::from(words[1].word()) << 32
}

fn count_of(len: usize) -> u32 {
    u32::try_from(len).expect("the tables of compiled code have fewer than 4 Gi words")
}

/// A frame for `ArtifactWriter::add_frame()`.
pub struct Frame {
    /// See `FrameView::executable()` and the functions after it.
    pub executable: u32,
    pub pc: u32,
    pub mode: ResumeMode,
    pub passed_argument_count: Option<u32>,
    pub legacy_arguments: Range<u32>,
    /// The record of the frame of the caller, for a frame of an inlined call.
    pub parent: Option<u32>,
}

/// What `ArtifactWriter::finish()` adds to the tables: what the compiler has as a whole when the code is complete.
#[derive(Default)]
pub struct Tail<'a> {
    /// See `Artifact::osr_entries()`. In pc order.
    pub osr_entries: &'a [(u32, u32)],
    pub osr_loop_back_edges: &'a [u32],
    pub osr_outer_loop_back_edges: &'a [u32],
    pub uncaught_handlers: &'a [u32],
    /// See `Artifact::invalidation_patches()`.
    pub invalidation_patches: &'a [(u32, JumpPatch)],
    pub embedded_cells: &'a [CellId],
    pub dependencies: &'a [Dependency],
}

/// Writes the tables of compiled code, in the work zone of a compile.
pub struct ArtifactWriter<'a> {
    sites: Table<'a>,
    frames: Table<'a>,
    calls: Shared<'a, 256>,
    slots: Shared<'a, 1024>,
    in_frame: Shared<'a, 256>,
    objects: Table<'a>,
    locations: Shared<'a, 1024>,
    wide: Shared<'a, 256>,
}

/// A table of the writer, which holds up to `limit` words. A compile that needs more fails.
struct Table<'a> {
    words: ZoneVec<'a, u32>,
    limit: usize,
}

impl<'a> Table<'a> {
    fn new_in(limit: usize, work: &'a Zone) -> Self {
        Self {
            words: ZoneVec::new_in(work),
            limit,
        }
    }

    fn try_push(&mut self, word: u32) -> Result<(), CompileFailure> {
        self.try_extend_from_slice(&[word])
    }

    fn try_extend_from_slice(&mut self, words: &[u32]) -> Result<(), CompileFailure> {
        if words.len() > self.limit - self.words.len() {
            return Err(CompileFailure::TooLarge);
        }
        self.words.extend_from_slice(words);
        Ok(())
    }

    fn truncate(&mut self, len: usize) {
        self.words.truncate(len);
    }
}

impl core::ops::Deref for Table<'_> {
    type Target = [u32];

    fn deref(&self) -> &[u32] {
        &self.words
    }
}

impl core::ops::DerefMut for Table<'_> {
    fn deref_mut(&mut self) -> &mut [u32] {
        &mut self.words
    }
}

/// A table in which a run of words that is equal to one from before takes no room of its own.
struct Shared<'a, const INDEX_SIZE: usize> {
    words: Table<'a>,
    /// Where runs are: for the run added last with each hash, its start plus one, or zero for none. The index finds
    /// nearly all of the repeats and never grows: a run that another one pushed out of its place is stored one more
    /// time if it comes again.
    index: [u32; INDEX_SIZE],
}

impl<'a, const INDEX_SIZE: usize> Shared<'a, INDEX_SIZE> {
    fn new_in(limit: usize, work: &'a Zone) -> Self {
        const { assert!(INDEX_SIZE.is_power_of_two() && INDEX_SIZE > 1) };
        Self {
            words: Table::new_in(limit, work),
            index: [0; INDEX_SIZE],
        }
    }

    /// Where the run that starts at `start` and ends where the words end now is from now on: at an equal run from
    /// before if one is known, in place of which this run goes away.
    fn share(&mut self, start: usize) -> u32 {
        let run = &self.words[start..];
        if run.is_empty() {
            return count_of(start);
        }
        let hash = run.iter().fold(0u64, |hash, word| {
            (hash.rotate_left(5) ^ u64::from(*word)).wrapping_mul(0x9E37_79B9_7F4A_7C15)
        });
        let place = (hash >> (u64::BITS - INDEX_SIZE.trailing_zeros())) as usize;
        if let Some(known) = self.index[place].checked_sub(1)
            && self.words[known as usize..start].starts_with(run)
        {
            self.words.truncate(start);
            return known;
        }
        self.index[place] = count_of(start) + 1;
        count_of(start)
    }
}

/// The most words the tables of one compile can have (see `ArtifactWriter::finish()`).
pub const MAX_WORDS: usize = 1 << 25;

impl<'a> ArtifactWriter<'a> {
    // NB: These are how much of each the tables of one compile can have. They are far above what the code of real
    //     functions needs, and a compile that needs more fails.
    const MAX_SITES: usize = 1 << 20;
    const MAX_FRAMES: usize = 1 << 20;
    const MAX_SLOTS: usize = 1 << 22;
    const MAX_OBJECTS: usize = 1 << 18;
    const MAX_LOCATIONS: usize = 1 << 24;
    const MAX_WIDE_VALUES: usize = 1 << 20;
    const MAX_CALL_WORDS: usize = Self::MAX_FRAMES * CALL_WORDS_WITH_OBJECTS;

    /// A writer with no tables yet, which keeps them in `work`.
    pub fn new_in(work: &'a Zone) -> Self {
        // NB: So where a call record starts fits in a link.
        const { assert!(ArtifactWriter::MAX_CALL_WORDS < NO_CALL as usize) };
        Self {
            sites: Table::new_in(Self::MAX_SITES * SITE_WORDS, work),
            frames: Table::new_in(Self::MAX_FRAMES * FRAME_WORDS, work),
            calls: Shared::new_in(Self::MAX_CALL_WORDS, work),
            slots: Shared::new_in(Self::MAX_SLOTS, work),
            in_frame: Shared::new_in(Self::MAX_SLOTS, work),
            objects: Table::new_in(Self::MAX_OBJECTS * OBJECT_WORDS, work),
            locations: Shared::new_in(Self::MAX_LOCATIONS, work),
            wide: Shared::new_in(Self::MAX_WIDE_VALUES * WIDE_WORDS, work),
        }
    }

    /// Adds a frame with values in `slots` and with `in_frame` (see `FrameView::in_frame()`), both in any order.
    /// Returns its record.
    pub fn add_frame(
        &mut self,
        frame: &Frame,
        slots: impl Iterator<Item = u32>,
        in_frame: &[u32],
    ) -> Result<u32, CompileFailure> {
        let slots_start = self.slots.words.len();
        for slot in slots {
            self.slots.words.try_push(slot)?;
        }
        self.slots.words[slots_start..].sort_unstable();
        let value_count = count_of(self.slots.words.len() - slots_start);
        let in_frame_start = self.in_frame.words.len();
        self.in_frame.words.try_extend_from_slice(in_frame)?;
        self.in_frame.words[in_frame_start..].sort_unstable();
        let (resumes_after, resume_destination) = match frame.mode {
            ResumeMode::ResumeAt => (0, 0),
            ResumeMode::ResumeAfter { dst } => (LINK_RESUMES_AFTER, dst),
        };
        // NB: No frame has that many arguments, so the word that stands for none is free.
        if frame.passed_argument_count == Some(NONE) {
            return Err(CompileFailure::TooLarge);
        }
        // NB: A reader gets an empty range of legacy arguments for a frame with no call record, which is all that an
        //     empty range says, wherever it starts.
        let is_plain = frame.executable == 0
            && frame.parent.is_none()
            && frame.passed_argument_count.is_none()
            && frame.legacy_arguments.is_empty();
        let call = if is_plain {
            NO_CALL
        } else {
            let mut words = [0; CALL_WORDS];
            words[CALL_EXECUTABLE] = frame.executable;
            words[CALL_PARENT] = frame.parent.unwrap_or(NONE);
            words[CALL_DEPTH] = match frame.parent {
                Some(parent) => self.call_of(parent).map_or(0, |call| call[CALL_DEPTH]) + 1,
                None => 0,
            };
            words[CALL_PASSED_ARGUMENT_COUNT] = frame.passed_argument_count.unwrap_or(NONE);
            words[CALL_LEGACY_ARGUMENTS_START] = frame.legacy_arguments.start;
            words[CALL_LEGACY_ARGUMENTS_END] = frame.legacy_arguments.end;
            let start = self.calls.words.len();
            self.calls.words.try_extend_from_slice(&words)?;
            self.calls.share(start)
        };
        let record = self.frames.len() / FRAME_WORDS;
        let mut words = [0; FRAME_WORDS];
        words[FRAME_PC] = frame.pc;
        words[FRAME_RESUME_DESTINATION] = resume_destination;
        words[FRAME_VALUE_COUNT] = value_count;
        words[FRAME_SLOTS] = self.slots.share(slots_start);
        words[FRAME_IN_FRAME] = self.in_frame.share(in_frame_start);
        words[FRAME_IN_FRAME_COUNT] = count_of(in_frame.len());
        words[FRAME_LINK] = call << LINK_CALL_SHIFT | resumes_after;
        self.frames.try_extend_from_slice(&words)?;
        Ok(count_of(record))
    }

    fn link_of(&self, frame: u32) -> u32 {
        self.frames[frame as usize * FRAME_WORDS + FRAME_LINK]
    }

    /// The call record of `frame`, if it has one, without the objects.
    fn call_of(&self, frame: u32) -> Option<&[u32]> {
        let call = self.link_of(frame) >> LINK_CALL_SHIFT;
        (call != NO_CALL).then(|| &self.calls.words[call as usize..][..CALL_WORDS])
    }

    /// Whether `frame` has the objects of the sites it is the innermost frame of (see `set_frame_objects()`).
    pub fn frame_has_objects(&self, frame: u32) -> bool {
        self.link_of(frame) & LINK_HAS_OBJECTS != 0
    }

    /// Gives `frame` the objects of the sites it is the innermost frame of, which is at least one, as the kind, the
    /// allocation and the number of properties of each. The locations of their properties follow those of the frames' values in the run
    /// of each site, in the same order.
    pub fn set_frame_objects(
        &mut self,
        frame: u32,
        objects: impl Iterator<Item = (VirtualObjectDescriptorKind, u32, u32)>,
    ) -> Result<(), CompileFailure> {
        let mut properties = 0;
        let mut ancestor = frame;
        while ancestor != NONE {
            properties += self.frames[ancestor as usize * FRAME_WORDS + FRAME_VALUE_COUNT];
            ancestor = self.call_of(ancestor).map_or(NONE, |call| call[CALL_PARENT]);
        }
        let start = self.objects.len() / OBJECT_WORDS;
        for (kind, allocation, property_count) in objects {
            let (kind, cell) = match kind {
                VirtualObjectDescriptorKind::Object { shape } => (OBJECT_KIND_OBJECT, shape),
                VirtualObjectDescriptorKind::Closure { sample_slot } => (OBJECT_KIND_CLOSURE, sample_slot),
            };
            let mut words = [0; OBJECT_WORDS];
            words[OBJECT_KIND] = kind;
            words[OBJECT_ALLOCATION] = allocation;
            words[OBJECT_CELL..][..2].copy_from_slice(&halves(cell.0));
            words[OBJECT_PROPERTIES] = properties;
            words[OBJECT_PROPERTY_COUNT] = property_count;
            self.objects.try_extend_from_slice(&words)?;
            properties = properties.checked_add(property_count).ok_or(CompileFailure::TooLarge)?;
        }
        let count = self.objects.len() / OBJECT_WORDS - start;

        // NB: The frame gets a call record of its own, because other frames share the one it has.
        let mut words = [0; CALL_WORDS_WITH_OBJECTS];
        match self.call_of(frame) {
            Some(call) => words[..CALL_WORDS].copy_from_slice(call),
            None => {
                words[CALL_PARENT] = NONE;
                words[CALL_PASSED_ARGUMENT_COUNT] = NONE;
            }
        }
        words[CALL_OBJECTS] = count_of(start);
        words[CALL_OBJECT_COUNT] = count_of(count);
        let call = count_of(self.calls.words.len());
        self.calls.words.try_extend_from_slice(&words)?;
        let link = &mut self.frames[frame as usize * FRAME_WORDS + FRAME_LINK];
        *link = call << LINK_CALL_SHIFT | LINK_HAS_OBJECTS | (*link & LINK_RESUMES_AFTER);
        Ok(())
    }

    /// The word for `location`.
    pub fn location(&mut self, location: ValueLocation) -> Result<u32, CompileFailure> {
        let (kind, repr, payload) = match location {
            ValueLocation::Register(register, repr) => (LOCATION_REGISTER, repr, u32::from(register)),
            ValueLocation::Stack(offset, repr) => {
                // NB: The shifts keep the bits of an offset that fits, and its sign.
                let shifted = offset << LOCATION_PAYLOAD_SHIFT;
                if shifted >> LOCATION_PAYLOAD_SHIFT != offset {
                    return Err(CompileFailure::TooLarge);
                }
                (LOCATION_STACK, repr, shifted.cast_unsigned() >> LOCATION_PAYLOAD_SHIFT)
            }
            ValueLocation::Frame(slot) => (LOCATION_FRAME, Repr::Tagged, slot),
            ValueLocation::Constant(bits) => (LOCATION_CONSTANT, Repr::Tagged, self.wide_value(bits)?),
            ValueLocation::ArgumentsObject { mapped } => (LOCATION_ARGUMENTS_OBJECT, Repr::Tagged, u32::from(mapped)),
            ValueLocation::VirtualObject(index) => (LOCATION_VIRTUAL_OBJECT, Repr::Tagged, index),
        };
        if payload >= LOCATION_PAYLOAD_LIMIT {
            return Err(CompileFailure::TooLarge);
        }
        Ok(kind | (repr as u32) << LOCATION_REPR_SHIFT | payload << LOCATION_PAYLOAD_SHIFT)
    }

    /// The index of `value` in the table of 64-bit values.
    fn wide_value(&mut self, value: u64) -> Result<u32, CompileFailure> {
        let start = self.wide.words.len();
        self.wide.words.try_extend_from_slice(&halves(value))?;
        Ok(self.wide.share(start) / count_of(WIDE_WORDS))
    }

    /// How many locations there are: where the run of the next site starts.
    pub fn location_count(&self) -> u32 {
        count_of(self.locations.words.len())
    }

    /// Adds the word of a location (see `location()`) to the run of the site that is added next.
    pub fn add_location(&mut self, word: u32) -> Result<(), CompileFailure> {
        self.locations.words.try_push(word)
    }

    /// Adds a site with `frame` as its innermost frame, whose run of locations starts at `locations` and ends where
    /// the locations end now. Returns its index.
    pub fn add_site(
        &mut self,
        kind: SiteKind,
        frame: u32,
        locations: u32,
        inlined_frame_bytes: u32,
    ) -> Result<u32, CompileFailure> {
        let index = self.sites.len() / SITE_WORDS;
        let kind = match kind {
            SiteKind::Exit(kind) => SITE_KIND_EXIT | (kind as u32) << 8,
            SiteKind::Leave => SITE_KIND_LEAVE,
            SiteKind::Publish => SITE_KIND_PUBLISH,
            SiteKind::Call => SITE_KIND_CALL,
        };
        let mut words = [0; SITE_WORDS];
        words[SITE_KIND] = kind;
        words[SITE_FRAME] = frame;
        words[SITE_LOCATIONS] = self.locations.share(locations as usize);
        words[SITE_INLINED_FRAME_BYTES] = inlined_frame_bytes;
        self.sites.try_extend_from_slice(&words)?;
        Ok(count_of(index))
    }

    /// Completes the tables with `tail`, and writes them as one array into `words` in place of what it holds, with
    /// the layout this returns.
    pub fn finish(&mut self, tail: &Tail<'_>, words: &mut Column<u32>) -> Result<Layout, CompileFailure> {
        let Self {
            sites,
            frames,
            calls,
            slots,
            in_frame,
            objects,
            locations,
            wide,
        } = self;
        words.clear();
        let mut layout = Layout::default();
        let mut next_section = 0;
        let mut end = |section: Section, words: &Column<u32>| {
            assert_eq!(
                section as usize, next_section,
                "the sections are written in their order"
            );
            layout.ends[section as usize] = count_of(words.len());
            next_section += 1;
        };

        words.try_extend_from_slice(sites)?;
        end(Section::Sites, words);
        words.try_extend_from_slice(frames)?;
        end(Section::Frames, words);
        words.try_extend_from_slice(&calls.words)?;
        end(Section::Calls, words);
        words.try_extend_from_slice(&slots.words)?;
        end(Section::Slots, words);
        words.try_extend_from_slice(&in_frame.words)?;
        end(Section::InFrame, words);
        words.try_extend_from_slice(objects)?;
        end(Section::Objects, words);
        words.try_extend_from_slice(&locations.words)?;
        end(Section::Locations, words);
        words.try_extend_from_slice(&wide.words)?;
        end(Section::Wide, words);

        // NB: The entries are in pc order in both sections, so the pair of an entry is at the same index of each.
        assert!(tail.osr_entries.is_sorted());
        for (pc, _) in tail.osr_entries {
            words.try_push(*pc)?;
        }
        end(Section::OsrPcs, words);
        for (_, offset) in tail.osr_entries {
            words.try_push(*offset)?;
        }
        end(Section::OsrOffsets, words);

        for (section, run) in [
            (Section::OsrLoopBackEdges, tail.osr_loop_back_edges),
            (Section::OsrOuterLoopBackEdges, tail.osr_outer_loop_back_edges),
            (Section::UncaughtHandlers, tail.uncaught_handlers),
        ] {
            let start = words.len();
            words.try_extend_from_slice(run)?;
            words[start..].sort_unstable();
            end(section, words);
        }

        for (offset, patch) in tail.invalidation_patches {
            let bytes = patch.bytes();
            let mut padded = [0; MAX_PATCH_LENGTH];
            padded[..bytes.len()].copy_from_slice(bytes);
            let mut record = [0; PATCH_WORDS];
            record[PATCH_OFFSET] = *offset;
            record[PATCH_LENGTH] = count_of(bytes.len());
            record[PATCH_BYTES..][..2].copy_from_slice(&halves(u64::from_le_bytes(padded)));
            words.try_extend_from_slice(&record)?;
        }
        end(Section::Patches, words);

        for cell in tail.embedded_cells {
            words.try_extend_from_slice(&halves(cell.0))?;
        }
        end(Section::EmbeddedCells, words);
        for dependency in tail.dependencies {
            let (kind, cell, value) = match *dependency {
                Dependency::WatchpointSetValid(cell) => (0, cell, 0),
                Dependency::GlobalDeclarations { environment, serial } => (1, environment, serial),
                Dependency::GlobalBindingUnassigned { environment, index } => (2, environment, u64::from(index)),
                Dependency::GlobalPropertyUnassigned { object, offset } => (3, object, u64::from(offset)),
                Dependency::NoHtmlDdaObjects => (4, CellId(0), 0),
                Dependency::StableShape(cell) => (5, cell, 0),
            };
            let mut record = [kind; DEPENDENCY_WORDS];
            record[1..3].copy_from_slice(&halves(cell.0));
            record[3..5].copy_from_slice(&halves(value));
            words.try_extend_from_slice(&record)?;
        }
        end(Section::Dependencies, words);
        assert_eq!(next_section, SECTION_COUNT);
        Ok(layout)
    }
}

/// The tables of compiled code in `words`, which have `layout`.
pub struct Artifact<'a, W = u32> {
    words: &'a [W],
    layout: &'a Layout,
}

impl<W> Clone for Artifact<'_, W> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<W> Copy for Artifact<'_, W> {}

impl<'a, W: Word> Artifact<'a, W> {
    pub fn new(words: &'a [W], layout: &'a Layout) -> Self {
        Self { words, layout }
    }

    fn section(self, section: Section) -> &'a [W] {
        &self.words[self.layout.range(section)]
    }

    fn record(self, section: Section, index: u32, words: usize) -> &'a [W] {
        &self.section(section)[index as usize * words..][..words]
    }

    fn records(self, section: Section, words: usize) -> impl ExactSizeIterator<Item = &'a [W]> {
        self.section(section).chunks_exact(words)
    }

    /// The call record of the frame with `record`, if it has one.
    fn call_of(self, record: &'a [W]) -> Option<&'a [W]> {
        let link = record[FRAME_LINK].word();
        let call = link >> LINK_CALL_SHIFT;
        let words = if link & LINK_HAS_OBJECTS == 0 {
            CALL_WORDS
        } else {
            CALL_WORDS_WITH_OBJECTS
        };
        (call != NO_CALL).then(|| &self.section(Section::Calls)[call as usize..][..words])
    }

    fn location(self, word: u32) -> ValueLocation {
        let payload = word >> LOCATION_PAYLOAD_SHIFT;
        let repr = Repr::ALL[((word >> LOCATION_REPR_SHIFT) & LOCATION_REPR_MASK) as usize];
        match word & LOCATION_KIND_MASK {
            LOCATION_REGISTER => ValueLocation::Register(payload as u8, repr),
            LOCATION_STACK => ValueLocation::Stack(
                (payload << LOCATION_PAYLOAD_SHIFT).cast_signed() >> LOCATION_PAYLOAD_SHIFT,
                repr,
            ),
            LOCATION_FRAME => ValueLocation::Frame(payload),
            LOCATION_CONSTANT => ValueLocation::Constant(from_halves(self.record(Section::Wide, payload, WIDE_WORDS))),
            LOCATION_ARGUMENTS_OBJECT => ValueLocation::ArgumentsObject { mapped: payload != 0 },
            LOCATION_VIRTUAL_OBJECT => ValueLocation::VirtualObject(payload),
            kind => unreachable!("{kind} is no kind of location"),
        }
    }

    pub fn site_count(self) -> u32 {
        count_of(self.section(Section::Sites).len() / SITE_WORDS)
    }

    /// The site with the index `index`: exit and leave stubs pass the index of theirs to the runtime, and calls in
    /// inlined callees name theirs.
    pub fn site(self, index: u32) -> SiteView<'a, W> {
        SiteView {
            artifact: self,
            record: self.record(Section::Sites, index, SITE_WORDS),
        }
    }

    pub fn sites(self) -> impl ExactSizeIterator<Item = SiteView<'a, W>> {
        self.records(Section::Sites, SITE_WORDS)
            .map(move |record| SiteView { artifact: self, record })
    }

    /// The on-stack replacement entry points, as (pc, offset within the code), in pc order: a frame running in the
    /// interpreter that is at the loop back edge `pc` can continue there, with the same calling convention as the
    /// entry point.
    pub fn osr_entries(self) -> impl ExactSizeIterator<Item = (u32, u32)> {
        let pcs = self.section(Section::OsrPcs).iter().map(W::word);
        pcs.zip(self.section(Section::OsrOffsets).iter().map(W::word))
    }

    /// The offset within the code of the on-stack replacement entry point at `pc`, if there is one.
    pub fn osr_entry(self, pc: u32) -> Option<u32> {
        let index = self
            .section(Section::OsrPcs)
            .binary_search_by(|word| word.word().cmp(&pc))
            .ok()?;
        Some(self.section(Section::OsrOffsets)[index].word())
    }

    /// The loop back edges from where a frame running in the interpreter reaches one of the `osr_entries()` without
    /// leaving the loop it is in.
    pub fn osr_loop_back_edges(self) -> Run<'a, W> {
        Run(self.section(Section::OsrLoopBackEdges))
    }

    /// The loop back edges in the loops around those of the `osr_entries()`, from where a frame running in the
    /// interpreter may reach one of them on a later iteration.
    pub fn osr_outer_loop_back_edges(self) -> Run<'a, W> {
        Run(self.section(Section::OsrOuterLoopBackEdges))
    }

    /// See `ir::Graph::uncaught_handlers`.
    pub fn uncaught_handlers(self) -> Run<'a, W> {
        Run(self.section(Section::UncaughtHandlers))
    }

    /// What invalidates the code: the bytes to write at each offset of it (see `Op::AssumeValid`).
    pub fn invalidation_patches(self) -> impl ExactSizeIterator<Item = Patch> {
        self.records(Section::Patches, PATCH_WORDS).map(|record| Patch {
            offset: record[PATCH_OFFSET].word(),
            length: record[PATCH_LENGTH].word() as usize,
            bytes: from_halves(&record[PATCH_BYTES..]).to_le_bytes(),
        })
    }

    /// Cells the code depends on (compares against or uses). The runtime keeps them alive for as long as the code
    /// may run. Only tables that have every section have them.
    pub fn embedded_cells(self) -> impl ExactSizeIterator<Item = CellId> {
        self.records(Section::EmbeddedCells, CELL_WORDS)
            .map(|record| CellId(from_halves(record)))
    }

    /// What the code assumes keeps holding. The runtime installs the code only if all of them still hold, and
    /// invalidates it when one stops holding. Only tables that have every section have them.
    pub fn dependencies(self) -> impl ExactSizeIterator<Item = Dependency> {
        self.records(Section::Dependencies, DEPENDENCY_WORDS).map(|record| {
            let cell = CellId(from_halves(&record[1..3]));
            let value = from_halves(&record[3..5]);
            match record[0].word() {
                0 => Dependency::WatchpointSetValid(cell),
                1 => Dependency::GlobalDeclarations {
                    environment: cell,
                    serial: value,
                },
                2 => Dependency::GlobalBindingUnassigned {
                    environment: cell,
                    index: value as u32,
                },
                3 => Dependency::GlobalPropertyUnassigned {
                    object: cell,
                    offset: value as u32,
                },
                4 => Dependency::NoHtmlDdaObjects,
                5 => Dependency::StableShape(cell),
                kind => unreachable!("{kind} is no kind of dependency"),
            }
        })
    }
}

/// Bytes to write into compiled code.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Patch {
    /// Where in the code the bytes go.
    pub offset: u32,
    length: usize,
    bytes: [u8; MAX_PATCH_LENGTH],
}

impl Patch {
    pub fn bytes(&self) -> &[u8] {
        &self.bytes[..self.length]
    }
}

/// A sorted run of words.
pub struct Run<'a, W = u32>(&'a [W]);

impl<W> Clone for Run<'_, W> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<W> Copy for Run<'_, W> {}

impl<'a, W: Word> Run<'a, W> {
    pub fn len(self) -> usize {
        self.0.len()
    }

    pub fn is_empty(self) -> bool {
        self.0.is_empty()
    }

    pub fn iter(self) -> impl DoubleEndedIterator<Item = u32> + ExactSizeIterator + Clone + use<'a, W> {
        self.0.iter().map(W::word)
    }

    pub fn contains(self, value: u32) -> bool {
        self.0.binary_search_by(|word| word.word().cmp(&value)).is_ok()
    }
}

/// A point of compiled code where the runtime may translate its state into interpreter frames (see `SiteKind`): the
/// frame states there, and where their values are.
pub struct SiteView<'a, W = u32> {
    artifact: Artifact<'a, W>,
    record: &'a [W],
}

impl<W> Clone for SiteView<'_, W> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<W> Copy for SiteView<'_, W> {}

impl<'a, W: Word> SiteView<'a, W> {
    pub fn kind(self) -> SiteKind {
        let word = self.record[SITE_KIND].word();
        match word & 0xff {
            SITE_KIND_EXIT => SiteKind::Exit(ExitKind::ALL[(word >> 8) as usize]),
            SITE_KIND_LEAVE => SiteKind::Leave,
            SITE_KIND_PUBLISH => SiteKind::Publish,
            SITE_KIND_CALL => SiteKind::Call,
            kind => unreachable!("{kind} is no kind of site"),
        }
    }

    /// For a `SiteKind::Call`: the room for the frames of the inlined calls that the call leaves below its callee's
    /// frame (see `Op::CallDirect`).
    pub fn inlined_frame_bytes(self) -> u32 {
        self.record[SITE_INLINED_FRAME_BYTES].word()
    }

    fn innermost_frame(self) -> &'a [W] {
        self.artifact
            .record(Section::Frames, self.record[SITE_FRAME].word(), FRAME_WORDS)
    }

    fn locations(self) -> &'a [W] {
        &self.artifact.section(Section::Locations)[self.record[SITE_LOCATIONS].word() as usize..]
    }

    /// How many frames are there: those of the inlined calls the site is in, and the compiled function's frame.
    pub fn frame_count(self) -> usize {
        let call = self.artifact.call_of(self.innermost_frame());
        call.map_or(0, |call| call[CALL_DEPTH].word()) as usize + 1
    }

    /// The frames there, innermost first.
    pub fn frames(self) -> Frames<'a, W> {
        Frames {
            artifact: self.artifact,
            next: self.record[SITE_FRAME].word(),
            locations: self.locations(),
        }
    }

    /// The frame at `index` of `frames()`.
    pub fn frame(self, index: usize) -> FrameView<'a, W> {
        self.frames().nth(index).expect("the site has the frame")
    }

    /// The words of the call record of the innermost frame that say where the objects start and how many there are.
    fn objects_of_innermost_frame(self) -> Option<&'a [W]> {
        let record = self.innermost_frame();
        let call = self.artifact.call_of(record)?;
        (record[FRAME_LINK].word() & LINK_HAS_OBJECTS != 0).then(|| &call[CALL_OBJECTS..])
    }

    pub fn object_count(self) -> u32 {
        self.objects_of_innermost_frame()
            .map_or(0, |objects| objects[CALL_OBJECT_COUNT - CALL_OBJECTS].word())
    }

    /// The object at `index` among the objects the compiled code never allocated, which the frames' values refer to
    /// (as `ValueLocation::VirtualObject`).
    pub fn object(self, index: u32) -> ObjectView<'a, W> {
        assert!(index < self.object_count());
        let objects = self.objects_of_innermost_frame().expect("the site has objects");
        let record = self
            .artifact
            .record(Section::Objects, objects[0].word() + index, OBJECT_WORDS);
        let properties = &self.locations()[record[OBJECT_PROPERTIES].word() as usize..]
            [..record[OBJECT_PROPERTY_COUNT].word() as usize];
        ObjectView {
            artifact: self.artifact,
            record,
            properties,
        }
    }

    pub fn objects(self) -> impl ExactSizeIterator<Item = ObjectView<'a, W>> {
        (0..self.object_count()).map(move |index| self.object(index))
    }
}

/// The frames of a site, innermost first.
pub struct Frames<'a, W = u32> {
    artifact: Artifact<'a, W>,
    /// The record of the next frame.
    next: u32,
    /// The locations of the site from those of the next frame on.
    locations: &'a [W],
}

impl<'a, W: Word> Iterator for Frames<'a, W> {
    type Item = FrameView<'a, W>;

    fn next(&mut self) -> Option<FrameView<'a, W>> {
        if self.next == NONE {
            return None;
        }
        let record = self.artifact.record(Section::Frames, self.next, FRAME_WORDS);
        let (locations, rest) = self.locations.split_at(record[FRAME_VALUE_COUNT].word() as usize);
        self.next = self
            .artifact
            .call_of(record)
            .map_or(NONE, |call| call[CALL_PARENT].word());
        self.locations = rest;
        Some(FrameView {
            artifact: self.artifact,
            record,
            locations,
        })
    }
}

/// The interpreter state of one frame at a site.
pub struct FrameView<'a, W = u32> {
    artifact: Artifact<'a, W>,
    record: &'a [W],
    /// Where the values are, in the order of their slots.
    locations: &'a [W],
}

impl<W> Clone for FrameView<'_, W> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<W> Copy for FrameView<'_, W> {}

impl<'a, W: Word> FrameView<'a, W> {
    /// Index into `Snapshot::executables`.
    pub fn executable(self) -> u32 {
        self.call().map_or(0, |call| call[CALL_EXECUTABLE].word())
    }

    fn call(self) -> Option<&'a [W]> {
        self.artifact.call_of(self.record)
    }

    pub fn pc(self) -> u32 {
        self.record[FRAME_PC].word()
    }

    pub fn mode(self) -> ResumeMode {
        if self.record[FRAME_LINK].word() & LINK_RESUMES_AFTER == 0 {
            ResumeMode::ResumeAt
        } else {
            ResumeMode::ResumeAfter {
                dst: self.record[FRAME_RESUME_DESTINATION].word(),
            }
        }
    }

    /// See `ir::FrameState::passed_argument_count`.
    pub fn passed_argument_count(self) -> Option<u32> {
        Some(self.call()?[CALL_PASSED_ARGUMENT_COUNT].word()).filter(|count| *count != NONE)
    }

    /// The argument slots that the legacy `arguments` of the frame's function shows (see
    /// `ir::FrameState::legacy_arguments`). Objects the code never allocated among them are only created when code
    /// reads them (see `ir::Op::AssumeLegacyArgumentsUnread`).
    pub fn legacy_arguments(self) -> Range<u32> {
        self.call().map_or(0..0, |call| {
            call[CALL_LEGACY_ARGUMENTS_START].word()..call[CALL_LEGACY_ARGUMENTS_END].word()
        })
    }

    /// The slots of `values()`.
    pub fn slots(self) -> Run<'a, W> {
        let slots = self.artifact.section(Section::Slots);
        Run(&slots[self.record[FRAME_SLOTS].word() as usize..][..self.locations.len()])
    }

    /// The value of each frame slot that differs from what the frame already holds, as the slot and where the value
    /// is, in slot order.
    pub fn values(self) -> impl DoubleEndedIterator<Item = (u32, ValueLocation)> + ExactSizeIterator + use<'a, W> {
        let locations = self
            .locations
            .iter()
            .map(move |word| self.artifact.location(word.word()));
        self.slots().iter().zip(locations)
    }

    /// Where the value of `slot` is, if it is among `values()`.
    pub fn value(self, slot: u32) -> Option<ValueLocation> {
        let index = self.slots().0.binary_search_by(|word| word.word().cmp(&slot)).ok()?;
        Some(self.artifact.location(self.locations[index].word()))
    }

    /// Where the function object of a frame of an inlined closure is (see `CALLEE_SLOT`).
    pub fn callee(self) -> Option<ValueLocation> {
        // NB: No slot is above this one.
        self.values()
            .next_back()
            .filter(|(slot, _)| *slot == CALLEE_SLOT)
            .map(|(_, location)| location)
    }

    /// The live slots whose value the frame already holds (see `ir::FrameState::in_frame`), in slot order. Exits
    /// clear the registers and locals that are neither among them nor among the values, except a `ResumeAfter`
    /// destination, which exits never write either.
    pub fn in_frame(self) -> Run<'a, W> {
        let in_frame = self.artifact.section(Section::InFrame);
        Run(&in_frame[self.record[FRAME_IN_FRAME].word() as usize..]
            [..self.record[FRAME_IN_FRAME_COUNT].word() as usize])
    }
}

/// An object the compiled code never allocated, which an exit creates as its kind says. Each one is created once per
/// exit, before any is given its properties, so objects may refer to each other.
pub struct ObjectView<'a, W = u32> {
    artifact: Artifact<'a, W>,
    record: &'a [W],
    properties: &'a [W],
}

impl<W> Clone for ObjectView<'_, W> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<W> Copy for ObjectView<'_, W> {}

impl<'a, W: Word> ObjectView<'a, W> {
    pub fn kind(self) -> VirtualObjectDescriptorKind {
        let cell = CellId(from_halves(&self.record[OBJECT_CELL..]));
        match self.record[OBJECT_KIND].word() {
            OBJECT_KIND_OBJECT => VirtualObjectDescriptorKind::Object { shape: cell },
            OBJECT_KIND_CLOSURE => VirtualObjectDescriptorKind::Closure { sample_slot: cell },
            kind => unreachable!("{kind} is no kind of object"),
        }
    }

    /// The node index of the allocation the object stands for, which is the same at every site of the code: the
    /// runtime may have created the object at another site of the same frame already, and then uses that.
    pub fn allocation(self) -> u32 {
        self.record[OBJECT_ALLOCATION].word()
    }

    /// Where the value at `index` of those the kind describes is, if the object has that many.
    pub fn property(self, index: usize) -> Option<ValueLocation> {
        self.properties
            .get(index)
            .map(|word| self.artifact.location(word.word()))
    }

    /// Where the values are that the kind describes.
    pub fn properties(self) -> impl ExactSizeIterator<Item = ValueLocation> + use<'a, W> {
        self.properties
            .iter()
            .map(move |word| self.artifact.location(word.word()))
    }
}
