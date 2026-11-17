/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A navigable's presenter, which keeps what the navigable's compositor context holds, and the presentation of one frame
//! of the navigable: the presenter and the seal of what the frame is built from, which what presents the frame beside
//! the event loop owns while it is away from the navigable.
//!
//! Only the owner of a frame's presentation presents it, on the Paint thread, so frames reach the compositor in the order
//! their presentations were presented. The navigable reads what the presenter presented on the main thread.

use crate::paint_stage::Presenting;
use crate::painting::display_list::chunks::{Chunk, ChunkId, KeptChunks};
use crate::painting::display_list::commands::CompositorContextId;
use crate::painting::display_list_resources::DisplayListResources;
use crate::painting::paint_passes::ClockTickVisualContexts;
use crate::painting::record::RecordingOutput;
use crate::painting::record::order_tree::RecordedChunk;
use crate::painting::vector_image_records::{VectorImageRecords, chunk_holding_vector_image_records};
use crate::painting::visual_context::VisualContextTree;
use libcompositing_rust::display_list::window::RecordedWindow;
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    ApplyScrollRequests, PresentFrame, UpdateDisplayList, UpdateScrollState, UpdateVisualContextTree,
};
use libcompositing_rust::ipc_types::{
    Compositing_AsyncScrollingMetadata, Compositing_DisplayListProperties, Compositing_KeyboardScrollState,
    Compositing_ScrollRequest,
};
use libcompositing_rust::scrolling::ScrollStateSnapshot;
use libgfx_rust::IntRect;
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libipc_rust::wire::{Decode, Decoder};
use std::collections::HashMap;
use std::sync::{Arc, Mutex, MutexGuard};

/// What presented a navigable's last frame: the recording of a frame a rendering update committed, or a tick of the
/// document's clock lane.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiPresentedBy {
    None,
    Commit,
    Clock,
}

/// `Web::HTML::PaintConfig`, which the presenter keeps for the display list the compositor context holds.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiPaintConfig {
    pub paint_overlay: bool,
    pub should_show_line_box_borders: bool,
    pub should_show_caret_hit_test_debug_overlay: bool,
    pub force_dark_enabled: bool,
    pub force_dark_foreground_threshold: i32,
    pub force_dark_background_threshold: i32,
    pub has_canvas_fill_rect: bool,
    pub canvas_fill_rect: IntRect,
}

/// The display list a navigable's compositor context holds, which the presenter presented last.
struct CompositorDisplayList {
    output: Arc<RecordingOutput>,
    /// What its commands draw, at any depth.
    command_resources: DisplayListResources,
}

/// What a navigable's presenter keeps of the frames it presented.
#[derive(Default)]
struct PresenterState {
    compositor_display_list: Option<CompositorDisplayList>,
    compositor_display_list_paint_config: Option<FfiPaintConfig>,
    last_frame_presented_by: Option<FfiPresentedBy>,
    last_keyboard_scroll_state_generation: Option<u64>,
    /// The generation of the last frame a rendering update committed that the presenter presented.
    last_committed_generation: u64,
    /// The scroll offsets the last frame the presenter presented sent the compositor.
    last_presented_scroll_state: Option<ScrollStateSnapshot>,
}

/// A navigable's presenter, which its navigable and what presents a frame of it beside the event loop share: the Paint
/// thread presents, and the main thread reads what it presented.
#[derive(Default)]
pub struct NavigablePresenter {
    state: Mutex<PresenterState>,
}

impl NavigablePresenter {
    fn state(&self) -> MutexGuard<'_, PresenterState> {
        self.state.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
    }
}

/// What a frame is built from that its navigable and document held where the frame began, sealed there: the frame reaches
/// neither of them again before it is presented.
pub(crate) struct Seal {
    paint_config: FfiPaintConfig,
    /// The structural epoch of the visual context tree a new display list is cut from, or of the one the compositor's
    /// display list takes where the document's tree changed since it took one.
    visual_context_tree_structural_epoch: Option<u64>,
    /// Whether the document's tree changed since the compositor took one.
    sends_visual_context_tree: bool,
    /// Whether presenting the frame sent the compositor other scroll offsets than the frame presented before it.
    presented_other_scroll_offsets: bool,
    /// The scrolls the navigable asked for since the frame before, which the compositor applies once it has the frame's
    /// boxes. Only the frame a rendering update commits carries them.
    scroll_requests: Vec<Compositing_ScrollRequest>,
    /// What keyboard scrolling targets, for a top-level traversable.
    keyboard_scroll_state: Option<Compositing_KeyboardScrollState>,
    /// The recording a recording of the frame copies paint commands from.
    paint_command_cache_source: Option<Arc<RecordingOutput>>,
    /// Whether a recording of the frame becomes the one the next recording copies from.
    publishes_recording: bool,
    context_id: CompositorContextId,
    present_viewport_rect: Option<IntRect>,
    presented_by: FfiPresentedBy,
    /// The recording the frame presented, once it is presented.
    published: Option<Arc<RecordingOutput>>,
    /// Whether the frame sends a visual context tree the compositor's display list was not recorded against, for a test.
    visual_context_tree_mismatches_for_testing: bool,
    /// The frame a rendering update committed, counted per navigable, or the one a clock lane's frames start from: a
    /// lane's frame of an older one than the presenter presented last shows what the screen no longer shows.
    generation: u64,
}

/// What C++ seals of a frame where it begins (see [`Seal`]).
#[repr(C)]
pub struct FfiSeal {
    pub presenter: *const NavigablePresenter,
    pub paint_config: FfiPaintConfig,
    pub has_visual_context_tree_structural_epoch: bool,
    pub visual_context_tree_structural_epoch: u64,
    pub sends_visual_context_tree: bool,
    /// The scroll requests, IPC-encoded as a `Vector<Compositing::ScrollRequest>`.
    pub encoded_scroll_requests: *const u8,
    pub encoded_scroll_requests_size: usize,
    /// The keyboard scroll state, IPC-encoded as an `Optional<Compositing::KeyboardScrollState>`.
    pub encoded_keyboard_scroll_state: *const u8,
    pub encoded_keyboard_scroll_state_size: usize,
    pub context_id: u64,
    pub has_present_viewport_rect: bool,
    pub present_viewport_rect: IntRect,
    pub generation: u64,
    pub visual_context_tree_mismatches_for_testing: bool,
}

/// Decodes `T` from `size` IPC-encoded bytes at `bytes`, which hold no attachment.
///
/// # Safety
///
/// `bytes` must point to `size` bytes, or be null with `size` 0.
unsafe fn decode_ipc_bytes<T: Decode>(bytes: *const u8, size: usize) -> T {
    // SAFETY: Guaranteed by the caller.
    let bytes = unsafe { ak::slice_from_raw_parts(bytes, size) };
    Decoder::new(bytes, std::iter::empty())
        .decode()
        .expect("C++ encodes what it seals")
}

/// What presents one frame of a navigable beside the event loop: the navigable's presenter, and what the frame is built
/// from.
pub(crate) struct Presentation {
    presenter: Arc<NavigablePresenter>,
    seal: Box<Seal>,
    /// Where the document's boxes were scrolled to as the frame was sealed, which seeds the offsets of the boxes new to
    /// the compositor.
    scroll_state: ScrollStateSnapshot,
    /// The visual context tree the frames presented from the seal take to the compositor where a clock tick's update
    /// left one, or the render state's where a frame without a recording sends it. A frame with a recording goes with
    /// the tree it was recorded against otherwise.
    taken_visual_context_tree: Option<Arc<VisualContextTree>>,
}

/// What one frame hands the compositor, in the order it hands it.
struct CompositorFrame {
    context_id: CompositorContextId,
    display_list_update: Option<DisplayListUpdate>,
    visual_context_tree_update: Option<VisualContextTreeUpdate>,
    scroll_state_update: Option<ScrollStateUpdate>,
    scroll_requests: Vec<Compositing_ScrollRequest>,
    present_viewport_rect: Option<IntRect>,
}

/// The fonts and the image frames that a display list or a visual context tree draws, which the compositor is told of
/// ahead of it.
struct DrawnResources {
    fonts: Vec<FontHandle>,
    image_frames: Vec<ImageFrameHandle>,
}

struct DisplayListUpdate {
    output: Arc<RecordingOutput>,
    properties: Compositing_DisplayListProperties,
    async_scrolling_metadata: Compositing_AsyncScrollingMetadata,
    visual_context_tree: Arc<VisualContextTree>,
    resources: DrawnResources,
    scroll_state: ScrollStateSnapshot,
}

struct VisualContextTreeUpdate {
    visual_context_tree: Arc<VisualContextTree>,
    resources: DrawnResources,
    scroll_state: ScrollStateSnapshot,
    keyboard_scroll_state: Compositing_KeyboardScrollState,
}

struct ScrollStateUpdate {
    scroll_state: ScrollStateSnapshot,
    keyboard_scroll_state: Compositing_KeyboardScrollState,
}

pub(crate) fn empty_keyboard_scroll_state() -> Compositing_KeyboardScrollState {
    Compositing_KeyboardScrollState {
        generation: 0,
        visual_context_tree_structural_epoch: 0,
        target: None,
        page_scroll_distance: 0.0,
        arrow_scroll_distance: 0.0,
    }
}

fn empty_async_scrolling_metadata() -> Compositing_AsyncScrollingMetadata {
    Compositing_AsyncScrollingMetadata {
        document_id: None,
        viewport_rect: IntRect::default(),
        wheel_event_listener_state_generation: 0,
        has_blocking_wheel_event_listeners: false,
        has_blocking_wheel_event_region_covering_viewport: false,
        viewport_keeps_horizontal_scrolls: false,
        viewport_keeps_vertical_scrolls: false,
        device_pixels_per_css_pixel: 1.0,
        keyboard_scroll_state: empty_keyboard_scroll_state(),
    }
}

/// What travels beside the tape of `output`'s display list.
pub(crate) fn display_list_properties_of(output: &RecordingOutput) -> Compositing_DisplayListProperties {
    Compositing_DisplayListProperties {
        id: output.display_list_id,
        compatible_visual_context_tree_structural_epoch: output.recorded_structural_epoch,
        surface_clear_color: output.surface_clear_color,
        root_group_color: output.root_group_color,
        window: output.window.as_deref().map(RecordedWindow::to_ipc),
    }
}

impl PresenterState {
    /// A clock lane's frame that starts from an older frame than the last one a rendering update committed shows what the
    /// screen no longer shows: it is not presented, and the compositor hears nothing of it.
    fn is_stale(&self, seal: &Seal) -> bool {
        seal.presented_by == FfiPresentedBy::Clock && seal.generation < self.last_committed_generation
    }

    fn note_presented(&mut self, seal: &mut Seal, scroll_state: &ScrollStateSnapshot) {
        seal.presented_other_scroll_offsets = !self
            .last_presented_scroll_state
            .as_ref()
            .is_some_and(|last| last.holds_the_offsets_of(scroll_state));
        if seal.presented_other_scroll_offsets {
            self.last_presented_scroll_state = Some(scroll_state.clone());
        }
        if seal.presented_by == FfiPresentedBy::Commit {
            self.last_committed_generation = self.last_committed_generation.max(seal.generation);
        }
    }

    /// The image frames that the filters of `visual_context_tree` draw and `drawn` lacks, as the display list the
    /// compositor has holds them.
    fn filter_image_frames_missing_from(
        &self,
        drawn: &DisplayListResources,
        visual_context_tree: &VisualContextTree,
    ) -> Vec<ImageFrameHandle> {
        let mut image_frames = Vec::new();
        visual_context_tree.for_each_effects_filter_image_frame_id(&mut |id| {
            if drawn.image_frames.contains_key(&id) {
                return;
            }
            let held = self
                .compositor_display_list
                .as_ref()
                .and_then(|display_list| display_list.command_resources.image_frames.get(&id));
            if let Some(image_frame) = held {
                image_frames.push(image_frame.clone());
            }
        });
        image_frames
    }

    /// Builds the frame that brings the compositor context `seal` names up to date with `published`, the display list a
    /// recording just published, or with what changed for the one the compositor has where none was published, from what
    /// `seal` sealed, the visual context tree the frame goes with, and the scroll state sealed beside it, which seeds the
    /// offsets of the boxes new to the compositor.
    fn build_frame(
        &mut self,
        seal: &Seal,
        published: Option<&Arc<RecordingOutput>>,
        visual_context_tree: &Arc<VisualContextTree>,
        scroll_state: &ScrollStateSnapshot,
    ) -> CompositorFrame {
        // Keyboard eligibility belongs to this publication, not to the cached paint commands. Refresh it even if recording
        // was skipped or repeated the display list, and send it with the corresponding scroll state.
        let display_list_output = published
            .or(self
                .compositor_display_list
                .as_ref()
                .map(|display_list| &display_list.output))
            .cloned();
        let mut keyboard_scroll_state = empty_keyboard_scroll_state();
        if let Some(sealed_state) = &seal.keyboard_scroll_state {
            keyboard_scroll_state = sealed_state.clone();
            self.last_keyboard_scroll_state_generation = Some(sealed_state.generation);
            keyboard_scroll_state.visual_context_tree_structural_epoch = display_list_output
                .as_ref()
                .map_or(0, |output| output.recorded_structural_epoch);
        }

        self.last_frame_presented_by = Some(seal.presented_by);
        let mut frame = CompositorFrame {
            context_id: seal.context_id,
            display_list_update: None,
            visual_context_tree_update: None,
            scroll_state_update: None,
            scroll_requests: Vec::new(),
            present_viewport_rect: seal.present_viewport_rect,
        };
        let display_list_is_unchanged = published.is_some_and(|published| {
            self.compositor_display_list
                .as_ref()
                .is_some_and(|held| held.output.display_list_id == published.display_list_id)
        });
        if let Some(published) = published.filter(|_| !display_list_is_unchanged) {
            let output = published.clone();
            let command_resources = output.resources.with_vector_image_resources();
            let mut image_frames: Vec<ImageFrameHandle> = command_resources.image_frames.values().cloned().collect();
            image_frames.extend(self.filter_image_frames_missing_from(&command_resources, visual_context_tree));
            let resources = DrawnResources {
                fonts: command_resources.fonts.values().cloned().collect(),
                image_frames,
            };
            let mut async_scrolling_metadata = output
                .async_scrolling_metadata
                .clone()
                .unwrap_or_else(empty_async_scrolling_metadata);
            async_scrolling_metadata.keyboard_scroll_state = keyboard_scroll_state;
            frame.display_list_update = Some(DisplayListUpdate {
                properties: display_list_properties_of(&output),
                async_scrolling_metadata,
                visual_context_tree: visual_context_tree.clone(),
                resources,
                scroll_state: scroll_state.clone(),
                output: output.clone(),
            });
            self.compositor_display_list = Some(CompositorDisplayList {
                output,
                command_resources,
            });
            self.compositor_display_list_paint_config = Some(seal.paint_config);
            return frame;
        }

        if display_list_is_unchanged {
            self.compositor_display_list_paint_config = Some(seal.paint_config);
        }
        if seal.sends_visual_context_tree {
            let held_epoch = self
                .compositor_display_list
                .as_ref()
                .map(|display_list| display_list.output.recorded_structural_epoch);
            assert!(
                seal.visual_context_tree_mismatches_for_testing
                    || held_epoch.is_none_or(|epoch| epoch == visual_context_tree.structural_epoch),
                "a frame sends a tree of the structure the compositor's display list was recorded against"
            );
            let resources = DrawnResources {
                fonts: Vec::new(),
                image_frames: self
                    .filter_image_frames_missing_from(&DisplayListResources::default(), visual_context_tree),
            };
            frame.visual_context_tree_update = Some(VisualContextTreeUpdate {
                visual_context_tree: visual_context_tree.clone(),
                resources,
                scroll_state: scroll_state.clone(),
                keyboard_scroll_state,
            });
        } else {
            frame.scroll_state_update = Some(ScrollStateUpdate {
                scroll_state: scroll_state.clone(),
                keyboard_scroll_state,
            });
        }
        frame
    }
}

impl Presentation {
    /// The presentation of a frame `ffi` sealed, with `scroll_state`, the scroll state of the document, and
    /// `paint_command_cache_source`, the recording a recording of the frame copies from, whose recording becomes the
    /// next one's source where `publishes_recording`.
    ///
    /// # Safety
    ///
    /// `ffi` must name a live presenter, and buffers of the sizes it gives.
    pub(crate) unsafe fn adopt(
        ffi: &FfiSeal,
        scroll_state: &ScrollStateSnapshot,
        paint_command_cache_source: Option<Arc<RecordingOutput>>,
        publishes_recording: bool,
    ) -> Self {
        // SAFETY: Guaranteed by the caller.
        let presenter = unsafe {
            Arc::increment_strong_count(ffi.presenter);
            Arc::from_raw(ffi.presenter)
        };
        Self {
            presenter,
            seal: Box::new(Seal {
                paint_config: ffi.paint_config,
                visual_context_tree_structural_epoch: ffi
                    .has_visual_context_tree_structural_epoch
                    .then_some(ffi.visual_context_tree_structural_epoch),
                sends_visual_context_tree: ffi.sends_visual_context_tree,
                presented_other_scroll_offsets: false,
                // SAFETY: Guaranteed by the caller.
                scroll_requests: unsafe {
                    decode_ipc_bytes(ffi.encoded_scroll_requests, ffi.encoded_scroll_requests_size)
                },
                // SAFETY: Guaranteed by the caller.
                keyboard_scroll_state: unsafe {
                    decode_ipc_bytes(
                        ffi.encoded_keyboard_scroll_state,
                        ffi.encoded_keyboard_scroll_state_size,
                    )
                },
                paint_command_cache_source,
                publishes_recording,
                context_id: CompositorContextId(ffi.context_id),
                present_viewport_rect: ffi.has_present_viewport_rect.then_some(ffi.present_viewport_rect),
                presented_by: FfiPresentedBy::Commit,
                published: None,
                visual_context_tree_mismatches_for_testing: ffi.visual_context_tree_mismatches_for_testing,
                generation: ffi.generation,
            }),
            scroll_state: scroll_state.clone(),
            taken_visual_context_tree: None,
        }
    }

    /// Whether presenting the frame sent the compositor other scroll offsets than the frame presented before it.
    pub(crate) fn presented_other_scroll_offsets(&self) -> bool {
        self.seal.presented_other_scroll_offsets
    }

    /// The structural epoch of the visual context tree the sealed frames go with: the one the compositor has, or the one
    /// a clock tick's update last left the seal.
    pub(crate) fn visual_context_tree_structural_epoch(&self) -> Option<u64> {
        self.seal.visual_context_tree_structural_epoch
    }

    /// What the clock lane of the frame this presented presents its frames with: the presenter, and a seal of frames
    /// that start from this one, or none where the compositor shows another display list than this frame records from.
    /// On the Paint thread, once this presented.
    pub(crate) fn for_clock_lane(&self) -> Option<Self> {
        let state = self.presenter.state();
        // A lane records the document again as the frame's recording did, or as the one an unrecorded frame keeps, which
        // the compositor has to show: a clock frame presented before the committed one may have taken its place.
        let committed = &self.seal;
        let source = committed
            .published
            .as_ref()
            .or(committed.paint_command_cache_source.as_ref())?;
        let held = state.compositor_display_list.as_ref()?;
        if held.output.display_list_id != source.display_list_id {
            return None;
        }
        let seal = Seal {
            paint_config: committed.paint_config,
            visual_context_tree_structural_epoch: Some(held.output.recorded_structural_epoch),
            sends_visual_context_tree: false,
            presented_other_scroll_offsets: false,
            scroll_requests: Vec::new(),
            keyboard_scroll_state: committed.keyboard_scroll_state.clone(),
            paint_command_cache_source: Some(held.output.clone()),
            publishes_recording: false,
            context_id: committed.context_id,
            present_viewport_rect: committed.present_viewport_rect,
            presented_by: FfiPresentedBy::Clock,
            published: None,
            visual_context_tree_mismatches_for_testing: false,
            generation: committed.generation,
        };
        drop(state);
        Some(Self {
            presenter: self.presenter.clone(),
            seal: Box::new(seal),
            scroll_state: self.scroll_state.clone(),
            taken_visual_context_tree: None,
        })
    }

    /// Has the frames presented from the seal take `visual_contexts` to the compositor, the tree with which a clock
    /// tick's update left the one the seal held.
    pub(crate) fn take_visual_context_tree(&mut self, visual_contexts: ClockTickVisualContexts) {
        let ClockTickVisualContexts {
            tree,
            restructured_scroll_offsets,
        } = visual_contexts;
        // A tree of another structure takes a new display list recorded against it, and the scroll offsets of its own
        // nodes. A frame that records nothing seals no tree, and takes the render state's: the compositor's display list
        // was cut from a tree of its structure.
        let tree_is_restructured = restructured_scroll_offsets.is_some();
        if let Some(scroll_offsets) = restructured_scroll_offsets {
            self.scroll_state = ScrollStateSnapshot::from_device_offsets(scroll_offsets);
        }
        if !tree_is_restructured && let Some(epoch) = self.seal.visual_context_tree_structural_epoch {
            assert_eq!(
                tree.structural_epoch, epoch,
                "a tree of the same structure keeps its structural epoch"
            );
        }
        self.seal.visual_context_tree_structural_epoch = Some(tree.structural_epoch);
        // A frame whose display list is the one the compositor has takes the tree on its own.
        self.seal.sends_visual_context_tree = true;
        self.taken_visual_context_tree = Some(tree);
    }

    /// Presents the sealed frame without a recording: the display list the compositor has stands, with
    /// `visual_context_tree`, the render state's tree, where the seal sends one.
    pub(crate) fn present_unrecorded(
        &mut self,
        visual_context_tree: Option<Arc<VisualContextTree>>,
        presenting: &mut Presenting,
    ) {
        if let Some(tree) = visual_context_tree {
            self.take_visual_context_tree(ClockTickVisualContexts {
                tree,
                restructured_scroll_offsets: None,
            });
        }
        let presenter = self.presenter.clone();
        let mut state = presenter.state();
        if state.is_stale(&self.seal) {
            return;
        }
        let tree = self
            .taken_visual_context_tree
            .clone()
            .or_else(|| {
                state
                    .compositor_display_list
                    .as_ref()
                    .and_then(|display_list| display_list.output.visual_context_tree.clone())
            })
            .unwrap_or_else(|| Arc::new(VisualContextTree::create_identity()));
        let mut frame = state.build_frame(&self.seal, None, &tree, &self.scroll_state);
        frame.scroll_requests = std::mem::take(&mut self.seal.scroll_requests);
        state.note_presented(&mut self.seal, &self.scroll_state);
        presenting.post_frame(frame);
    }

    /// Presents the sealed frame with the recording that left `output`. The frame goes with the tree a clock tick's update
    /// left, or else the one the recording was recorded against: a recording that repeats the display list it published
    /// before publishes that list again.
    pub(crate) fn present(&mut self, output: &Arc<RecordingOutput>, presenting: &mut Presenting) {
        let tree = self
            .taken_visual_context_tree
            .clone()
            .or_else(|| output.visual_context_tree.clone())
            .expect("a recording was recorded against a tree");
        debug_assert_eq!(
            tree.structural_epoch, output.recorded_structural_epoch,
            "a frame goes with a tree of the structure its recording was recorded against"
        );
        let presenter = self.presenter.clone();
        let mut state = presenter.state();
        if state.is_stale(&self.seal) {
            return;
        }
        let replaces_paint_command_cache_source = self.seal.publishes_recording
            && !self
                .seal
                .paint_command_cache_source
                .as_ref()
                .is_some_and(|source| source.display_list_id == output.display_list_id);
        let mut frame = state.build_frame(&self.seal, Some(output), &tree, &self.scroll_state);
        if replaces_paint_command_cache_source {
            self.seal.paint_command_cache_source = Some(output.clone());
        }
        self.seal.published = Some(output.clone());
        frame.scroll_requests = std::mem::take(&mut self.seal.scroll_requests);
        state.note_presented(&mut self.seal, &self.scroll_state);
        presenting.post_frame(frame);
    }
}

/// What a display list or a visual context tree draws goes ahead of it, in messages the C++ of the compositor takes,
/// which C++ encodes. Answers whether the compositor could still be reached.
fn post_resources_ahead(
    poster: &crate::paint_stage::CompositorPoster,
    context_id: CompositorContextId,
    resources: &DrawnResources,
) -> bool {
    poster.post_fonts_and_image_frames(context_id, &resources.fonts, &resources.image_frames)
}

impl Presenting<'_> {
    /// Hands `frame` to the compositor, or drops it while the compositor cannot be reached.
    fn post_frame(&mut self, frame: CompositorFrame) {
        let Some((poster, sent_display_list_chunks)) = self.poster_and_sent_display_list_chunks() else {
            return;
        };
        if !post_frame_through(poster, sent_display_list_chunks, frame) {
            self.lose_compositor();
        }
    }
}

/// What the compositor keeps of the display lists sent for one context, which a display list sends the chunks it lacks
/// of, and the chunks that draw SVG images as they were sent, with the records of the images in them.
#[derive(Default)]
pub(crate) struct SentDisplayListChunks {
    kept: KeptChunks,
    holding_vector_image_records: HashMap<ChunkId, Arc<Chunk>>,
}

impl SentDisplayListChunks {
    /// `chunk` as the compositor replays it, with the records of `vector_images`, the SVG images it draws, in it.
    fn as_sent(&mut self, chunk: &RecordedChunk, vector_images: &HashMap<u64, Arc<VectorImageRecords>>) -> Arc<Chunk> {
        let latest = chunk.latest();
        if !chunk.draws_vector_image_placeholders() {
            return Arc::clone(latest);
        }
        // A chunk's name says which SVG images it draws, whose records never change.
        Arc::clone(
            self.holding_vector_image_records
                .entry(latest.id)
                .or_insert_with(|| Arc::new(chunk_holding_vector_image_records(latest, vector_images))),
        )
    }

    /// The chunks of `chunks_as_sent` the compositor does not keep, once each.
    fn new_chunks<'a>(&self, chunks_as_sent: &'a [Arc<Chunk>]) -> Vec<&'a Chunk> {
        let mut named = std::collections::HashSet::new();
        chunks_as_sent
            .iter()
            .filter(|chunk| !self.kept.keeps(chunk.id) && named.insert(chunk.id))
            .map(|chunk| &**chunk)
            .collect()
    }

    /// Takes in a display list sent that names `chunks_in_paint_order`.
    fn take_in(&mut self, chunks_in_paint_order: &[ChunkId]) {
        self.kept.take_in(chunks_in_paint_order);
        let kept = &self.kept;
        self.holding_vector_image_records.retain(|_, sent| kept.keeps(sent.id));
    }
}

/// Posts the messages of `frame` through `poster`, in order, where `sent_display_list_chunks` says what the compositor
/// keeps of the display lists sent before. Answers whether the compositor could still be reached: a display list that
/// cannot be placed in shared memory is skipped, and the rest of the frame still goes over.
fn post_frame_through(
    poster: &crate::paint_stage::CompositorPoster,
    sent_display_list_chunks: &mut HashMap<CompositorContextId, SentDisplayListChunks>,
    frame: CompositorFrame,
) -> bool {
    let CompositorFrame {
        context_id,
        display_list_update,
        visual_context_tree_update,
        scroll_state_update,
        scroll_requests,
        present_viewport_rect,
    } = frame;
    if let Some(update) = display_list_update {
        if !post_resources_ahead(poster, context_id, &update.resources) {
            return false;
        }
        // The compositor replays chunks with the records of the SVG images they draw in them.
        let sent = sent_display_list_chunks.entry(context_id).or_default();
        let vector_images = &update.output.resources.vector_images;
        let chunks_as_sent: Vec<Arc<Chunk>> = update
            .output
            .chunks
            .iter()
            .map(|chunk| sent.as_sent(chunk, vector_images))
            .collect();
        let chunks_in_paint_order: Vec<ChunkId> = chunks_as_sent.iter().map(|chunk| chunk.id).collect();
        let new_chunks = sent.new_chunks(&chunks_as_sent);
        match poster.share_display_list(
            chunks_in_paint_order.clone(),
            &new_chunks,
            update.properties,
            Some(update.async_scrolling_metadata),
        ) {
            Some(display_list) => {
                let message = UpdateDisplayList {
                    context_id,
                    display_list,
                    visual_context_tree: update.visual_context_tree,
                    scroll_state: update.scroll_state,
                };
                if !poster.post(message) {
                    return false;
                }
                sent.take_in(&chunks_in_paint_order);
            }
            None => {
                eprintln!(
                    "WebContent: Could not place {} new display list chunks in shared memory",
                    new_chunks.len()
                );
            }
        }
    }
    if let Some(update) = visual_context_tree_update {
        if !post_resources_ahead(poster, context_id, &update.resources) {
            return false;
        }
        let message = UpdateVisualContextTree {
            context_id,
            visual_context_tree: update.visual_context_tree,
            scroll_state: update.scroll_state,
            keyboard_scroll_state: update.keyboard_scroll_state,
        };
        if !poster.post(message) {
            return false;
        }
    }
    if let Some(update) = scroll_state_update {
        let message = UpdateScrollState {
            context_id,
            scroll_state: update.scroll_state,
            keyboard_scroll_state: update.keyboard_scroll_state,
        };
        if !poster.post(message) {
            return false;
        }
    }
    if !scroll_requests.is_empty()
        && !poster.post(ApplyScrollRequests {
            context_id,
            scroll_requests,
        })
    {
        return false;
    }
    if let Some(viewport_rect) = present_viewport_rect
        && !poster.post(PresentFrame {
            context_id,
            viewport_rect,
        })
    {
        return false;
    }
    true
}

/// A new presenter, with one reference the caller takes over.
#[unsafe(no_mangle)]
pub extern "C" fn navigable_presenter_create() -> *const NavigablePresenter {
    Arc::into_raw(Arc::new(NavigablePresenter::default()))
}

/// Gives up a reference to `presenter`.
///
/// # Safety
///
/// `presenter` must be a reference to a presenter the caller holds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn navigable_presenter_release(presenter: *const NavigablePresenter) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Arc::from_raw(presenter) });
}

/// Writes the paint config of the display list the compositor context holds to `out`, and answers whether it holds one.
#[unsafe(no_mangle)]
pub extern "C" fn navigable_presenter_compositor_display_list_paint_config(
    presenter: &NavigablePresenter,
    out: &mut FfiPaintConfig,
) -> bool {
    let Some(paint_config) = presenter.state().compositor_display_list_paint_config else {
        return false;
    };
    *out = paint_config;
    true
}

/// Writes the structural epoch of the visual context tree the display list the compositor context holds was recorded
/// against to `out`, and answers whether it holds one.
#[unsafe(no_mangle)]
pub extern "C" fn navigable_presenter_compositor_display_list_structural_epoch(
    presenter: &NavigablePresenter,
    out: &mut u64,
) -> bool {
    let state = presenter.state();
    let Some(display_list) = state.compositor_display_list.as_ref() else {
        return false;
    };
    *out = display_list.output.recorded_structural_epoch;
    true
}

/// Forgets what the compositor context holds: a new compositor process holds nothing.
#[unsafe(no_mangle)]
pub extern "C" fn navigable_presenter_forget_compositor_display_list(presenter: &NavigablePresenter) {
    let mut state = presenter.state();
    state.compositor_display_list_paint_config = None;
    state.compositor_display_list = None;
}

/// What presented the navigable's last frame.
#[unsafe(no_mangle)]
pub extern "C" fn navigable_presenter_last_frame_presented_by(presenter: &NavigablePresenter) -> FfiPresentedBy {
    presenter
        .state()
        .last_frame_presented_by
        .unwrap_or(FfiPresentedBy::None)
}

/// Writes the generation of the keyboard scroll state the last frame handed the compositor to `out`, and answers
/// whether it handed one.
#[unsafe(no_mangle)]
pub extern "C" fn navigable_presenter_last_keyboard_scroll_state_generation(
    presenter: &NavigablePresenter,
    out: &mut u64,
) -> bool {
    let Some(generation) = presenter.state().last_keyboard_scroll_state_generation else {
        return false;
    };
    *out = generation;
    true
}
