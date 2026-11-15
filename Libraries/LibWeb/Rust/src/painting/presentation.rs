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
use crate::painting::display_list::commands::CompositorContextId;
use crate::painting::display_list::storage::DisplayListStorage;
use crate::painting::display_list_resources::{DisplayListResources, ResourceIds};
use crate::painting::paint_passes::ClockTickVisualContexts;
use crate::painting::record::RecordingOutput;
use crate::painting::visual_context::VisualContextTree;
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    ApplyScrollRequests, PresentFrame, UpdateDisplayList, UpdateScrollState, UpdateVisualContextTree,
};
use libcompositing_rust::ipc_types::{
    Compositing_AsyncScrollingMetadata, Compositing_DisplayListProperties, Compositing_DisplayListResourceTransaction,
    Compositing_KeyboardScrollState, Compositing_ScrollRequest,
};
use libcompositing_rust::scrolling::ScrollStateSnapshot;
use libgfx_rust::IntRect;
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libipc_rust::wire::{Decode, Decoder};
use std::collections::HashSet;
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
    /// The resources the compositor context holds.
    compositor_resource_ids: ResourceIds,
    last_frame_presented_by: Option<FfiPresentedBy>,
    last_keyboard_scroll_state_generation: Option<u64>,
    /// The generation of the last frame a rendering update committed that the presenter presented.
    last_committed_generation: u64,
    /// The scroll offsets the last frame the presenter presented sent the compositor.
    last_presented_scroll_state: Option<ScrollStateSnapshot>,
    /// What the display list of the last frame a rendering update committed draws, which the frames of a clock lane keep
    /// beside their own.
    committed_command_resources: DisplayListResources,
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

/// A resource transaction, with the fonts and image frames the compositor lacks, which go ahead of it.
struct ResourceUpdate {
    fonts: Vec<FontHandle>,
    image_frames: Vec<ImageFrameHandle>,
    transaction: Compositing_DisplayListResourceTransaction,
}

struct DisplayListUpdate {
    output: Arc<RecordingOutput>,
    properties: Compositing_DisplayListProperties,
    async_scrolling_metadata: Compositing_AsyncScrollingMetadata,
    visual_context_tree: Arc<VisualContextTree>,
    resources: ResourceUpdate,
    scroll_state: ScrollStateSnapshot,
}

struct VisualContextTreeUpdate {
    visual_context_tree: Arc<VisualContextTree>,
    resources: ResourceUpdate,
    scroll_state: ScrollStateSnapshot,
    keyboard_scroll_state: Compositing_KeyboardScrollState,
}

struct ScrollStateUpdate {
    scroll_state: ScrollStateSnapshot,
    keyboard_scroll_state: Compositing_KeyboardScrollState,
}

/// A display list a frame presents, and whether it becomes the one the next recording copies from.
struct Published<'a> {
    output: &'a Arc<RecordingOutput>,
    replaces_paint_command_cache_source: bool,
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
    }
}

/// Resources a compositor context keeps for a frame: those `resources` holds, and the image frames of `extra_image_frame_ids`
/// it holds already.
struct KeptResources {
    resources: DisplayListResources,
    extra_image_frame_ids: HashSet<u64>,
}

impl KeptResources {
    fn ids(&self) -> ResourceIds {
        let mut ids = self.resources.ids();
        ids.image_frames.extend(self.extra_image_frame_ids.iter().copied());
        ids
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
            self.committed_command_resources = self
                .compositor_display_list
                .as_ref()
                .map(|display_list| display_list.command_resources.clone())
                .unwrap_or_default();
        }
    }

    /// The resources the compositor keeps for a frame of `seal` whose display list draws `command_resources`, with
    /// `visual_context_tree`.
    fn kept_resources(
        &self,
        seal: &Seal,
        published: Option<&Published>,
        command_resources: &DisplayListResources,
        visual_context_tree: &VisualContextTree,
    ) -> KeptResources {
        let mut resources = command_resources.clone();
        // A recording downgraded to cache-read-only leaves the source it copied from live, and what it draws with it. A
        // recording that replaced the source draws everything the new source does.
        if !published.is_some_and(|published| published.replaces_paint_command_cache_source)
            && let Some(source) = &seal.paint_command_cache_source
        {
            resources.include(&source.resources.with_nested_resources());
        }
        // The next frame a rendering update commits copies paint commands from the one it committed last, whatever a
        // clock lane presented since.
        if seal.presented_by == FfiPresentedBy::Clock {
            resources.include(&self.committed_command_resources);
        }
        let mut extra_image_frame_ids = HashSet::new();
        visual_context_tree.for_each_effects_filter_image_frame_id(&mut |id| {
            if resources.image_frames.contains_key(&id) {
                return;
            }
            let handle = [
                published.map(|published| &published.output.resources),
                self.compositor_display_list
                    .as_ref()
                    .map(|display_list| &display_list.command_resources),
            ]
            .into_iter()
            .flatten()
            .find_map(|resources| resources.image_frames.get(&id).cloned());
            match handle {
                Some(handle) => {
                    resources.image_frames.insert(id, handle);
                }
                None if self.compositor_resource_ids.image_frames.contains(&id) => {
                    extra_image_frame_ids.insert(id);
                }
                None => {}
            }
        });
        KeptResources {
            resources,
            extra_image_frame_ids,
        }
    }

    /// What the compositor is sent to hold `kept` in place of what it holds.
    fn resource_update(&mut self, kept: KeptResources) -> ResourceUpdate {
        let held = std::mem::take(&mut self.compositor_resource_ids);
        let kept_ids = kept.ids();
        let KeptResources { resources, .. } = kept;
        let mut update = ResourceUpdate {
            fonts: Vec::new(),
            image_frames: Vec::new(),
            transaction: Compositing_DisplayListResourceTransaction::default(),
        };
        for (id, font) in resources.fonts {
            if !held.fonts.contains(&id) {
                update.fonts.push(font);
            }
        }
        for (id, frame) in resources.image_frames {
            if !held.image_frames.contains(&id) {
                update.image_frames.push(frame);
            }
        }
        for (id, display_list) in resources.nested_display_lists {
            if !held.display_lists.contains(&id) {
                update.transaction.display_lists.push(display_list.resource.clone());
            }
        }
        let removed =
            |held: &HashSet<u64>, kept: &HashSet<u64>| -> Vec<u64> { held.difference(kept).copied().collect() };
        update.transaction.font_ids_to_remove = removed(&held.fonts, &kept_ids.fonts);
        update.transaction.image_frame_ids_to_remove = removed(&held.image_frames, &kept_ids.image_frames);
        update.transaction.display_list_ids_to_remove = removed(&held.display_lists, &kept_ids.display_lists);
        self.compositor_resource_ids = kept_ids;
        update
    }

    /// Builds the frame that brings the compositor context `seal` names up to date with `published`, the display list a
    /// recording just published, or with what changed for the one the compositor has where none was published, from what
    /// `seal` sealed, the visual context tree the frame goes with, and the scroll state sealed beside it, which seeds the
    /// offsets of the boxes new to the compositor.
    fn build_frame(
        &mut self,
        seal: &Seal,
        published: Option<Published>,
        visual_context_tree: &Arc<VisualContextTree>,
        scroll_state: &ScrollStateSnapshot,
    ) -> CompositorFrame {
        // Keyboard eligibility belongs to this publication, not to the cached paint commands. Refresh it even if recording
        // was skipped or repeated the display list, and send it with the corresponding scroll state.
        let display_list_output = published
            .as_ref()
            .map(|published| published.output)
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
        let display_list_is_unchanged = published.as_ref().is_some_and(|published| {
            self.compositor_display_list
                .as_ref()
                .is_some_and(|held| held.output.display_list_id == published.output.display_list_id)
        });
        if let Some(published) = published.as_ref().filter(|_| !display_list_is_unchanged) {
            let output = published.output.clone();
            let command_resources = output.resources.with_nested_resources();
            let kept = self.kept_resources(seal, Some(published), &command_resources, visual_context_tree);
            let mut async_scrolling_metadata = output
                .async_scrolling_metadata
                .clone()
                .unwrap_or_else(empty_async_scrolling_metadata);
            async_scrolling_metadata.keyboard_scroll_state = keyboard_scroll_state;
            frame.display_list_update = Some(DisplayListUpdate {
                properties: display_list_properties_of(&output),
                async_scrolling_metadata,
                visual_context_tree: visual_context_tree.clone(),
                resources: self.resource_update(kept),
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
            let command_resources = self
                .compositor_display_list
                .as_ref()
                .map(|display_list| display_list.command_resources.clone())
                .unwrap_or_default();
            let kept = self.kept_resources(seal, None, &command_resources, visual_context_tree);
            frame.visual_context_tree_update = Some(VisualContextTreeUpdate {
                visual_context_tree: visual_context_tree.clone(),
                resources: self.resource_update(kept),
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
        let mut frame = state.build_frame(
            &self.seal,
            Some(Published {
                output,
                replaces_paint_command_cache_source,
            }),
            &tree,
            &self.scroll_state,
        );
        if replaces_paint_command_cache_source {
            self.seal.paint_command_cache_source = Some(output.clone());
        }
        self.seal.published = Some(output.clone());
        frame.scroll_requests = std::mem::take(&mut self.seal.scroll_requests);
        state.note_presented(&mut self.seal, &self.scroll_state);
        presenting.post_frame(frame);
    }
}

/// The fonts and image frames of a resource transaction go ahead of it, in messages the C++ of the compositor takes,
/// which C++ encodes. Answers whether the compositor could still be reached.
fn post_resources_ahead(
    poster: &crate::paint_stage::CompositorPoster,
    context_id: CompositorContextId,
    resources: &ResourceUpdate,
) -> bool {
    poster.post_fonts_and_image_frames(context_id, &resources.fonts, &resources.image_frames)
}

impl Presenting<'_> {
    /// Hands `frame` to the compositor, or drops it while the compositor cannot be reached.
    fn post_frame(&mut self, frame: CompositorFrame) {
        let Some(poster) = self.poster() else {
            return;
        };
        if !post_frame_through(poster, frame) {
            self.lose_compositor();
        }
    }
}

/// Posts the messages of `frame` through `poster`, in order. Answers whether the compositor could still be reached: a
/// display list that cannot be placed in shared memory is skipped, and the rest of the frame still goes over.
fn post_frame_through(poster: &crate::paint_stage::CompositorPoster, frame: CompositorFrame) -> bool {
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
        let storage = DisplayListStorage::recorded(
            update.output.display_list.clone(),
            update
                .output
                .visual_context_tree
                .clone()
                .unwrap_or_else(|| Arc::new(VisualContextTree::create_identity())),
        );
        match poster.share_display_list(&storage, update.properties, Some(update.async_scrolling_metadata)) {
            Some(display_list) => {
                let message = UpdateDisplayList {
                    context_id,
                    display_list,
                    visual_context_tree: update.visual_context_tree,
                    resource_transaction: update.resources.transaction,
                    scroll_state: update.scroll_state,
                };
                if !poster.post(message) {
                    return false;
                }
            }
            None => {
                eprintln!(
                    "WebContent: Could not place a {} byte display list in shared memory",
                    storage.bytes().len()
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
            resource_transaction: update.resources.transaction,
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
    state.compositor_resource_ids = ResourceIds::default();
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
