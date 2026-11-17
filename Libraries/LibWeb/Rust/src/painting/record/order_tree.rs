/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a recording keeps of the paint order it recorded, for the next recording to reuse clean output from.
//!
//! The planner's scopes nest to the depth of the document, and keeping every one of them costs more than the display
//! list they describe. So only some scopes are kept as nodes of their own: the ones a recording walks, because they
//! are too tall to be recorded in one piece, scroll, or may paint anywhere. The plan of a walked scope is cut into
//! runs of consecutive items, each recorded whole into a chunk of its own, with the walked child scopes between them.
//! A recording reuses the chunk of every clean run and every walked scope that is clean all the way down, and shares
//! those nodes with the recording it took them from.

use crate::css::css_pixels::CssPixelPoint;
use crate::painting::display_list::chunks::Chunk;
use crate::painting::paint_order_plan::{PaintOrderItem, PaintScope};
use crate::painting::record::window::InkBound;
use std::ops::Range;
use std::sync::{Arc, OnceLock};

/// One unit of recorded output owned by a row. The order of the draw phases mirrors `PaintPhase`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub(crate) enum ProducerKind {
    DrawBackground = 0,
    DrawBorder,
    DrawTableCollapsedBorder,
    DrawForeground,
    DrawOutline,
    DrawOverlay,
    ScrollMetadata,
    ScopePreamble,
    Svg,
    InlinePiece(u32),
    TextFragment(u32),
}

/// What one part of a recording recorded, besides its chunks, that decides whether it can be reused.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct OutputFacts {
    pub blocking_wheel_event_regions: u32,
    // Recorded again every frame: scroll-offset-dependent content and resources without
    // retained output. A scope is live when any child is.
    pub live: bool,
    // Recorded again when the window gains or loses focus. A scope reads the window focus when
    // any child does.
    pub reads_window_focus: bool,
    // Recorded again when a transform changes a scale that the spaces of the tree accumulate. A scope reads the
    // accumulated scale when any child does.
    pub reads_accumulated_scale: bool,
}

impl OutputFacts {
    pub(crate) fn add(&mut self, other: OutputFacts) {
        self.blocking_wheel_event_regions += other.blocking_wheel_event_regions;
        self.live |= other.live;
        self.reads_window_focus |= other.reads_window_focus;
        self.reads_accumulated_scale |= other.reads_accumulated_scale;
    }
}

/// Where a row's boxes are, as far as an ancestor can move them without anything stamping the row: the
/// origin of its first fragment, and for a row in several fragments a digest of where the later ones are, which a
/// change of the columns they sit in moves on its own.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct RowPlacement {
    pub origin: CssPixelPoint,
    pub later_fragments: u64,
}

/// A part of a walked scope's plan.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Segment {
    /// Consecutive items recorded as one piece: producers of the scope's owner, and child scopes with everything
    /// below them.
    Run(Range<u32>),
    /// The item that is a child scope walked on its own.
    Walked(u32),
}

/// The plan of a walked scope with where it is cut. The items follow paint order alone, and are shared with the
/// plan they were kept from while that holds. The cuts follow where the items are, so a recording that walks the
/// scope after anything in it changed or moved cuts it again.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct SegmentedPlan {
    pub active: bool,
    pub items: Arc<[PaintOrderItem]>,
    pub segments: Vec<Segment>,
}

impl SegmentedPlan {
    pub(crate) fn items_of(&self, run: &Range<u32>) -> &[PaintOrderItem] {
        &self.items[run.start as usize..run.end as usize]
    }

    pub(crate) fn walked_scope(&self, item: u32) -> PaintScope {
        match self.items[item as usize] {
            PaintOrderItem::Scope(scope) => scope,
            PaintOrderItem::Producer(_) => unreachable!("a walked segment names a child scope"),
        }
    }
}

/// The chunk one recording recorded. Where the chunk draws an SVG image, it holds a placeholder for the records the
/// host renders the image as, until the recording that made it is published and finishes it with their id: only a
/// finished chunk can be drawn by another recording.
#[derive(Debug)]
pub(crate) struct RecordedChunk {
    recorded: Arc<Chunk>,
    draws_vector_image_placeholders: bool,
    finished: OnceLock<Arc<Chunk>>,
}

impl RecordedChunk {
    pub(crate) fn new(recorded: Chunk, draws_vector_image_placeholders: bool) -> Arc<Self> {
        Arc::new(Self {
            recorded: Arc::new(recorded),
            draws_vector_image_placeholders,
            finished: OnceLock::new(),
        })
    }

    /// The chunk as the recording that made it recorded it.
    pub(crate) fn recorded(&self) -> &Arc<Chunk> {
        &self.recorded
    }

    pub(crate) fn draws_vector_image_placeholders(&self) -> bool {
        self.draws_vector_image_placeholders
    }

    /// The chunk as a display list draws it, where nothing in it is a placeholder any more.
    pub(crate) fn finished(&self) -> Option<&Arc<Chunk>> {
        match self.draws_vector_image_placeholders {
            false => Some(&self.recorded),
            true => self.finished.get(),
        }
    }

    /// The chunk as the recording that made it, or the publication of that recording, left it.
    pub(crate) fn latest(&self) -> &Arc<Chunk> {
        self.finished().unwrap_or(&self.recorded)
    }

    /// Finishes a chunk that draws placeholders with the chunk the publication of its recording patched them in.
    pub(crate) fn finish(&self, finished: Chunk) -> &Arc<Chunk> {
        debug_assert!(
            self.draws_vector_image_placeholders,
            "only a chunk with placeholders is finished"
        );
        let _ = self.finished.set(Arc::new(finished));
        self.finished.get().expect("the chunk was finished above")
    }
}

/// What a run recorded, and where the owners of its child scopes were when it did: a child that an ancestor moved
/// since records again though nothing stamped it.
#[derive(Debug)]
pub(crate) struct RecordedRun {
    /// None where the run recorded nothing.
    pub chunk: Option<Arc<RecordedChunk>>,
    pub facts: OutputFacts,
    pub child_scope_placements: Box<[RowPlacement]>,
}

#[derive(Debug)]
pub(crate) enum RecordedSegment {
    Run(RecordedRun),
    Walked(Arc<RecordedScope>),
    /// Nothing of the segment was recorded, because all of it lay outside the window of the recording.
    Outside,
}

impl RecordedSegment {
    pub(crate) fn facts(&self) -> OutputFacts {
        match self {
            Self::Run(run) => run.facts,
            Self::Walked(scope) => scope.facts,
            Self::Outside => OutputFacts::default(),
        }
    }

    pub(crate) fn leaves_out_what_lies_outside_the_window(&self) -> bool {
        match self {
            Self::Run(_) => false,
            Self::Walked(scope) => scope.leaves_out_what_lies_outside_the_window,
            Self::Outside => true,
        }
    }
}

/// A walked scope as one recording recorded it.
#[derive(Debug)]
pub(crate) struct RecordedScope {
    pub scope: PaintScope,
    pub plan: Arc<SegmentedPlan>,
    /// One for each segment of the plan.
    pub segments: Vec<RecordedSegment>,
    pub facts: OutputFacts,
    /// Where the scope's owner was, which the producers of the owner in the scope's runs were recorded against.
    pub owner_placement: RowPlacement,
    /// What bounds each segment, or none for a segment that may paint anywhere, where the recording had to ask:
    /// it holds a window of the document that the scope is not wholly inside.
    pub segment_bounds: Option<Arc<[Option<InkBound>]>>,
    /// Whether a segment of the scope, or of a scope walked below it, was left out for lying outside the window.
    pub leaves_out_what_lies_outside_the_window: bool,
    /// Whether a scope walked below this one belongs to a box that cuts off some of what it holds, as a scroller
    /// does: such a scope paints outside what bounds this one, so a recording of a window may leave some of it out
    /// though this scope is wholly inside the window.
    pub walks_a_scope_of_a_box_that_cuts_off_what_it_holds: bool,
}

impl RecordedScope {
    /// Hands `visit` the chunks of the scope in paint order.
    pub(crate) fn for_each_chunk(&self, visit: &mut impl FnMut(&Arc<RecordedChunk>)) {
        for segment in &self.segments {
            match segment {
                RecordedSegment::Run(run) => {
                    if let Some(chunk) = &run.chunk {
                        visit(chunk);
                    }
                }
                RecordedSegment::Walked(scope) => scope.for_each_chunk(visit),
                RecordedSegment::Outside => {}
            }
        }
    }
}
