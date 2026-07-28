/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const PAGE_BITS: usize = 4;
const PAGE_SIZE: usize = 1 << PAGE_BITS;
const PAGE_MASK: usize = PAGE_SIZE - 1;
const BUMP_ARENA_CHUNK_SIZE: usize = 4 * 1024;

struct ArenaChunk<T> {
    entries: Box<[MaybeUninit<T>]>,
    initialized: usize,
}

impl<T> ArenaChunk<T> {
    fn new() -> Self {
        let entry_count = (BUMP_ARENA_CHUNK_SIZE / size_of::<T>()).max(1);
        let entries = std::iter::repeat_with(MaybeUninit::uninit)
            .take(entry_count)
            .collect::<Vec<_>>()
            .into_boxed_slice();
        Self {
            entries,
            initialized: 0,
        }
    }

    fn is_full(&self) -> bool {
        self.initialized == self.entries.len()
    }

    fn allocate(&mut self, value: T) -> *mut T {
        assert!(!self.is_full());
        let entry = &mut self.entries[self.initialized];
        self.initialized += 1;
        entry.write(value)
    }
}

impl<T> Drop for ArenaChunk<T> {
    fn drop(&mut self) {
        for entry in &mut self.entries[..self.initialized] {
            // SAFETY: The prefix ending at `initialized` is written exactly
            // once by allocate(), and no entry is moved out of the arena.
            unsafe {
                entry.assume_init_drop();
            }
        }
    }
}

struct BumpArena<T> {
    chunks: Vec<ArenaChunk<T>>,
}

impl<T> Default for BumpArena<T> {
    fn default() -> Self {
        Self { chunks: Vec::new() }
    }
}

impl<T> BumpArena<T> {
    fn allocate(&mut self, value: T) -> *mut T {
        if self.chunks.last().is_none_or(ArenaChunk::is_full) {
            self.chunks.push(ArenaChunk::new());
        }
        self.chunks.last_mut().unwrap().allocate(value)
    }
}

struct Page<T> {
    entries: [*mut T; PAGE_SIZE],
}

impl<T> Default for Page<T> {
    fn default() -> Self {
        Self {
            entries: [null_mut(); PAGE_SIZE],
        }
    }
}

pub(crate) struct PagedStore<T> {
    inner: RefCell<PagedStoreInner<T>>,
}

struct PagedStoreInner<T> {
    pages: Vec<Option<Box<Page<T>>>>,
    arena: BumpArena<T>,
}

impl<T> Default for PagedStore<T> {
    fn default() -> Self {
        Self {
            inner: RefCell::new(PagedStoreInner {
                pages: Vec::new(),
                arena: BumpArena::default(),
            }),
        }
    }
}

impl<T> PagedStore<T> {
    fn entry_pointer(inner: &PagedStoreInner<T>, index: u32) -> *mut T {
        let index = index as usize;
        let page_index = index >> PAGE_BITS;
        let Some(Some(page)) = inner.pages.get(page_index) else {
            return null_mut();
        };
        page.entries[index & PAGE_MASK]
    }

    #[inline]
    pub(crate) fn get(&self, index: u32) -> Option<&T> {
        let inner = self.inner.borrow();
        let entry = Self::entry_pointer(&inner, index);
        if entry.is_null() {
            return None;
        }
        // SAFETY: Arena entries are Box-stable, never moved or freed before
        // the store, and the returned borrow is tied to the store.
        Some(unsafe { &*entry })
    }

    pub(crate) fn allocate(&self, index: u32, value: T) -> &T {
        let index = index as usize;
        let page_index = index >> PAGE_BITS;
        let mut inner = self.inner.borrow_mut();
        if page_index >= inner.pages.len() {
            inner.pages.resize_with(page_index + 1, || None);
        }
        let entry_index = index & PAGE_MASK;
        assert!(
            inner.pages[page_index]
                .as_ref()
                .is_none_or(|page| page.entries[entry_index].is_null())
        );
        let allocated = inner.arena.allocate(value);
        let page = inner.pages[page_index].get_or_insert_with(|| Box::new(Page::default()));
        page.entries[entry_index] = allocated;
        // SAFETY: The allocation is initialized and remains at this address
        // until the store is dropped.
        unsafe { &*allocated }
    }

    fn for_each_indexed(&self, mut callback: impl FnMut(u32, &T)) {
        let entries = {
            let inner = self.inner.borrow();
            let mut entries = Vec::new();
            for (page_index, page) in inner.pages.iter().enumerate() {
                let Some(page) = page else {
                    continue;
                };
                for (entry_index, entry) in page.entries.iter().copied().enumerate() {
                    if !entry.is_null() {
                        entries.push(((page_index * PAGE_SIZE + entry_index) as u32, entry));
                    }
                }
            }
            entries
        };
        for (index, entry) in entries {
            // SAFETY: Every collected pointer belongs to this append-only
            // arena and stays live for the whole store lifetime.
            callback(index, unsafe { &*entry });
        }
    }

    pub(crate) fn for_each(&self, mut callback: impl FnMut(&T)) {
        self.for_each_indexed(|_, value| callback(value));
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum StaticPositionAlignment {
    Start,
    Center,
    End,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StaticPositionRect {
    pub(crate) rect: LogicalRect,
    pub(crate) inline_alignment: StaticPositionAlignment,
    pub(crate) block_alignment: StaticPositionAlignment,
    pub(crate) alignment_derives_from_own_computed_values: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AbsposAxisMode {
    StaticPosition,
    InsetFromRect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AbsposAlignment {
    Baseline,
    Center,
    End,
    Normal,
    Safe,
    SelfEnd,
    SelfStart,
    SpaceAround,
    SpaceBetween,
    SpaceEvenly,
    Start,
    Stretch,
    Unsafe,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposContainingBlockInfo {
    pub(crate) rect: LogicalRect,
    pub(crate) inline_axis_mode: AbsposAxisMode,
    pub(crate) block_axis_mode: AbsposAxisMode,
    pub(crate) inline_alignment: Option<AbsposAlignment>,
    pub(crate) block_alignment: Option<AbsposAlignment>,
    pub(crate) derives_from_own_computed_values: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposLayoutInputs {
    pub(crate) static_position_rect: StaticPositionRect,
    pub(crate) containing_block_info: AbsposContainingBlockInfo,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiTableCellCoordinates {
    pub row_index: usize,
    pub column_index: usize,
    pub row_span: usize,
    pub column_span: usize,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommittedBoxMetrics {
    pub content_inline_size: crate::layout::CssPixels,
    pub content_block_size: crate::layout::CssPixels,
    pub margin_left: crate::layout::CssPixels,
    pub margin_right: crate::layout::CssPixels,
    pub margin_top: crate::layout::CssPixels,
    pub margin_bottom: crate::layout::CssPixels,
    pub border_left: crate::layout::CssPixels,
    pub border_right: crate::layout::CssPixels,
    pub border_top: crate::layout::CssPixels,
    pub border_bottom: crate::layout::CssPixels,
    pub padding_left: crate::layout::CssPixels,
    pub padding_right: crate::layout::CssPixels,
    pub padding_top: crate::layout::CssPixels,
    pub padding_bottom: crate::layout::CssPixels,
    pub inset_left: crate::layout::CssPixels,
    pub inset_right: crate::layout::CssPixels,
    pub inset_top: crate::layout::CssPixels,
    pub inset_bottom: crate::layout::CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommittedOffset {
    pub offset: crate::layout::FfiCssPixelPoint,
    pub inset_left: crate::layout::CssPixels,
    pub inset_top: crate::layout::CssPixels,
    pub materialized_from_paintable: bool,
    pub has_containing_line_box_fragment: bool,
    pub containing_line_box_index: usize,
    pub line_fragment_lookup: FfiLineFragmentLookup,
    pub line_fragment_offset: crate::layout::FfiCssPixelPoint,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiLineFragmentLookup {
    #[default]
    NotFound = 0,
    LineOnly = 1,
    Found = 2,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommitNodeResult {
    pub paintable: *mut c_void,
    pub paintable_for_children: *mut c_void,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCommitPosition {
    pub parent_paintable: *mut c_void,
    pub insert_before_paintable: *mut c_void,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiPaintableGeometry {
    pub content_inline_size: crate::layout::CssPixels,
    pub content_block_size: crate::layout::CssPixels,
    pub content_offset: crate::layout::FfiCssPixelPoint,
    pub margin_left: crate::layout::CssPixels,
    pub margin_right: crate::layout::CssPixels,
    pub margin_top: crate::layout::CssPixels,
    pub margin_bottom: crate::layout::CssPixels,
    pub border_left: crate::layout::CssPixels,
    pub border_right: crate::layout::CssPixels,
    pub border_top: crate::layout::CssPixels,
    pub border_bottom: crate::layout::CssPixels,
    pub padding_left: crate::layout::CssPixels,
    pub padding_right: crate::layout::CssPixels,
    pub padding_top: crate::layout::CssPixels,
    pub padding_bottom: crate::layout::CssPixels,
    pub inset_left: crate::layout::CssPixels,
    pub inset_right: crate::layout::CssPixels,
    pub inset_top: crate::layout::CssPixels,
    pub inset_bottom: crate::layout::CssPixels,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCommitSink {
    pub context: *mut c_void,
    pub begin_commit: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiCommitPosition,
    pub finish_commit: unsafe extern "C" fn(*mut c_void),
    pub prepare_node: unsafe extern "C" fn(*mut c_void, *mut c_void, bool) -> *mut c_void,
    pub set_box_metrics: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiCommittedBoxMetrics),
    pub set_override_borders: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiBordersData),
    pub set_table_cell_coordinates: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiTableCellCoordinates),
    pub begin_line_data: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub begin_line: unsafe extern "C" fn(*mut c_void, FfiLineRecord),
    pub emit_fragment: unsafe extern "C" fn(*mut c_void, FfiCommittedFragment),
    pub emit_inline_box_piece: unsafe extern "C" fn(*mut c_void, FfiInlineBoxPiece),
    pub finish_line_data: unsafe extern "C" fn(*mut c_void),
    pub set_computed_svg_transforms: unsafe extern "C" fn(*mut c_void, *mut c_void, crate::layout::FfiSvgComputedTransforms),
    pub set_computed_svg_path: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub set_grid_layout_data: unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiGridLayoutData),
    pub set_flex_layout_data: unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiFlexLayoutData),
    pub set_used_grid_tracks:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiUsedGridTrackList, *const FfiUsedGridTrackList),
    pub set_offset: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, FfiCommittedOffset),
    pub finish_node:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut c_void, *mut c_void) -> FfiCommitNodeResult,
    pub resolve_relative_positions: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

pub(crate) type ReleaseRetainedLayoutHandle = unsafe extern "C" fn(*mut c_void, *mut c_void);

pub(crate) struct RetainedLayoutHandle {
    handle: *mut c_void,
    callback_context: *mut c_void,
    release: ReleaseRetainedLayoutHandle,
}

impl RetainedLayoutHandle {
    pub(crate) fn new(
        handle: *mut c_void,
        callback_context: *mut c_void,
        release: ReleaseRetainedLayoutHandle,
    ) -> Self {
        assert!(!handle.is_null());
        Self {
            handle,
            callback_context,
            release,
        }
    }

    pub(crate) fn take(&mut self) -> *mut c_void {
        let handle = self.handle;
        self.handle = null_mut();
        handle
    }
}

impl Drop for RetainedLayoutHandle {
    fn drop(&mut self) {
        if self.handle.is_null() {
            return;
        }
        // SAFETY: Each retained layout handle is returned with one ownership
        // unit by its creating callback and is either transferred to the
        // commit sink or released exactly once with the paired callback.
        unsafe {
            (self.release)(self.callback_context, self.handle);
        }
    }
}

#[derive(Clone, Copy)]
struct ContainedAbsposChild {
    child_box: Node,
    static_position_rect: StaticPositionRect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PendingAbsposChild {
    pub(crate) child_box: Node,
    pub(crate) static_position_rect: StaticPositionRect,
}

/// A pass-scoped lens over one node's facts. Pure classification reads the
/// arena NodeData directly; style-derived answers read the per-slot style
/// snapshot; content-derived answers read the kind-gated lazy fact stores.
/// Nothing is materialized per node, so every answer is as live as its source.
#[derive(Clone, Copy)]
pub(crate) struct NodeFacts<'pass> {
    state: &'pass LayoutState,
    callbacks: &'pass FfiLayoutFcCallbacks,
    node: Node,
}

impl<'pass> NodeFacts<'pass> {
    fn data(&self) -> &'pass NodeData {
        self.callbacks.node_data(self.node)
    }

    fn parent_data(&self) -> Option<&'pass NodeData> {
        let parent = self.data().parent;
        (!parent.is_invalid()).then(|| self.callbacks.node_data(parent))
    }

    fn style(&self) -> StyleValues<'pass> {
        self.state.style_facts(self.callbacks, self.node)
    }

    fn replaced_content(&self) -> crate::layout::FfiReplacedContentFacts {
        self.state.replaced_content_facts(self.callbacks, self.node)
    }

    fn list_item(&self) -> crate::layout::FfiListItemFacts {
        self.state.list_item_facts(self.callbacks, self.node)
    }

    pub(crate) fn is_text_node(&self) -> bool {
        crate::layout::kind_is_text(self.data().kind)
    }

    pub(crate) fn is_break_node(&self) -> bool {
        self.data().kind == NodeKind::BreakNode
    }

    pub(crate) fn is_box(&self) -> bool {
        crate::layout::kind_is_box(self.data().kind)
    }

    pub(crate) fn is_block_container(&self) -> bool {
        crate::layout::kind_is_block_container(self.data().kind)
    }

    pub(crate) fn is_replaced_box(&self) -> bool {
        crate::layout::kind_is_replaced_box(self.data().kind)
    }

    pub(crate) fn is_replaced_box_with_children(&self) -> bool {
        let data = self.data();
        crate::layout::kind_is_replaced_box(data.kind) && crate::layout::node_can_have_children(data)
    }

    pub(crate) fn is_floating(&self) -> bool {
        crate::layout::has_display_flag(self.data(), NodeDisplayFlag::Floating)
    }

    pub(crate) fn is_absolutely_positioned(&self) -> bool {
        crate::layout::has_display_flag(self.data(), NodeDisplayFlag::AbsolutelyPositioned)
    }

    pub(crate) fn is_inline(&self) -> bool {
        let data = self.data();
        crate::layout::kind_is_text(data.kind) || crate::layout::has_display_flag(data, NodeDisplayFlag::InlineOutside)
    }

    pub(crate) fn is_atomic_inline(&self) -> bool {
        let data = self.data();
        crate::layout::has_flag(data, NodeFlag::IsReplacedElement)
            || data.kind == NodeKind::ListItemMarkerBox
            || (crate::layout::has_display_flag(data, NodeDisplayFlag::InlineOutside)
                && !crate::layout::has_display_flag(data, NodeDisplayFlag::FlowInside))
    }

    pub(crate) fn has_box_model_metrics(&self) -> bool {
        !matches!(
            self.data().kind,
            NodeKind::Unset
                | NodeKind::Node
                | NodeKind::NodeWithStyle
                | NodeKind::GeneratedTextNode
                | NodeKind::TextNode
                | NodeKind::TextSliceNode
        )
    }

    pub(crate) fn is_fragmented_inline(&self) -> bool {
        let data = self.data();
        data.kind == NodeKind::InlineNode
            || (data.kind == NodeKind::ListItemBox
                && crate::layout::has_display_flag(data, NodeDisplayFlag::InlineOutside)
                && crate::layout::has_display_flag(data, NodeDisplayFlag::FlowInside))
    }

    pub(crate) fn is_inline_flow_interrupting_block(&self) -> bool {
        if !self.has_box_model_metrics() {
            return false;
        }
        let data = self.data();
        let Some(parent) = self.parent_data() else {
            return false;
        };
        if !crate::layout::has_display_flag(parent, NodeDisplayFlag::InlineOutside)
            || !crate::layout::has_display_flag(parent, NodeDisplayFlag::FlowInside)
        {
            return false;
        }
        if crate::layout::has_display_flag(data, NodeDisplayFlag::InlineOutside) || crate::layout::node_is_out_of_flow(data) {
            return false;
        }
        if self.display().is_contents() {
            return false;
        }
        if !matches!(data.table_display, FfiTableDisplay::Other | FfiTableDisplay::TableRoot) {
            return false;
        }
        if parent.kind == NodeKind::SVGForeignObjectBox {
            return false;
        }
        if crate::layout::kind_is_svg_box(data.kind) || data.kind == NodeKind::SVGForeignObjectBox {
            return false;
        }
        if data.kind == NodeKind::SVGSVGBox
            && (crate::layout::kind_is_svg_box(parent.kind) || parent.kind == NodeKind::SVGSVGBox)
        {
            return false;
        }
        if crate::layout::kind_is_replaced_box(parent.kind) && crate::layout::node_can_have_children(parent) {
            return false;
        }
        true
    }

    pub(crate) fn is_list_item_marker_box(&self) -> bool {
        self.data().kind == NodeKind::ListItemMarkerBox
    }

    pub(crate) fn is_list_item_box(&self) -> bool {
        self.data().kind == NodeKind::ListItemBox
    }

    pub(crate) fn is_svg_mask_box(&self) -> bool {
        self.data().kind == NodeKind::SVGMaskBox
    }

    pub(crate) fn is_svg_clip_box(&self) -> bool {
        self.data().kind == NodeKind::SVGClipBox
    }

    pub(crate) fn display_before_box_type_transformation_is_block_outside(&self) -> bool {
        crate::layout::has_display_flag(self.data(), NodeDisplayFlag::BlockOutsideBeforeBoxTypeTransformation)
    }

    pub(crate) fn inline_axis_is_reverse(&self) -> bool {
        let style = self.style();
        match style.writing_mode {
            writing_mode::HORIZONTAL_TB
            | writing_mode::VERTICAL_RL
            | writing_mode::VERTICAL_LR
            | writing_mode::SIDEWAYS_RL => style.direction == 1,
            writing_mode::SIDEWAYS_LR => style.direction == 0,
            _ => unreachable!("invalid writing mode"),
        }
    }

    pub(crate) fn has_dom_node(&self) -> bool {
        !crate::layout::has_flag(self.data(), NodeFlag::Anonymous)
    }

    pub(crate) fn is_generated_for_pseudo_element(&self) -> bool {
        self.data().generated_for != 0
    }

    pub(crate) fn children_are_inline(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::ChildrenAreInline)
    }

    pub(crate) fn is_anonymous(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::Anonymous)
    }

    pub(crate) fn can_have_children(&self) -> bool {
        crate::layout::node_can_have_children(self.data())
    }

    pub(crate) fn has_replaced_element_table_display_adjustment(&self) -> bool {
        let data = self.data();
        crate::layout::has_flag(data, NodeFlag::IsReplacedElement) && data.table_display_before != FfiTableDisplay::Other
    }

    pub(crate) fn creates_block_formatting_context(&self) -> bool {
        crate::layout::node_creates_block_formatting_context(self.data(), self.parent_data())
    }

    pub(crate) fn is_grid_item(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsGridItem)
    }

    pub(crate) fn is_editing_host(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsEditingHost)
    }

    pub(crate) fn uses_button_layout(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::UsesButtonLayout)
    }

    pub(crate) fn vertical_align_applies(&self) -> bool {
        let data = self.data();
        crate::layout::kind_is_box(data.kind)
            && !crate::layout::has_flag(data, NodeFlag::IsFlexItem)
            && !crate::layout::has_flag(data, NodeFlag::IsGridItem)
    }

    pub(crate) fn is_html_input_element(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsHtmlInputElement)
    }

    pub(crate) fn is_fieldset_box(&self) -> bool {
        self.data().kind == NodeKind::FieldSetBox
    }

    pub(crate) fn rendered_legend(&self) -> Node {
        let data = self.data();
        if data.kind != NodeKind::FieldSetBox {
            return Node::INVALID;
        }
        let mut child = data.first_child;
        while !child.is_invalid() {
            let child_data = self.callbacks.node_data(child);
            if child_data.kind == NodeKind::LegendBox && !crate::layout::node_is_out_of_flow(child_data) {
                return child;
            }
            child = child_data.next_sibling;
        }
        Node::INVALID
    }

    pub(crate) fn list_item_marker(&self) -> Node {
        self.list_item().marker
    }

    pub(crate) fn has_css_marker_content(&self) -> bool {
        self.list_item().has_css_marker_content
    }

    pub(crate) fn marker_content_inline_size(&self) -> crate::layout::CssPixels {
        self.list_item().marker_content_inline_size
    }

    pub(crate) fn marker_content_block_size(&self) -> crate::layout::CssPixels {
        self.list_item().marker_content_block_size
    }

    pub(crate) fn marker_distance(&self) -> crate::layout::CssPixels {
        self.list_item().marker_distance
    }

    pub(crate) fn marker_list_style_position(&self) -> u8 {
        self.list_item().marker_list_style_position
    }

    pub(crate) fn has_auto_content_width(&self) -> bool {
        self.replaced_content().has_auto_content_width
    }

    pub(crate) fn auto_content_width(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_width
    }

    pub(crate) fn has_auto_content_height(&self) -> bool {
        self.replaced_content().has_auto_content_height
    }

    pub(crate) fn auto_content_height(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_height
    }

    pub(crate) fn has_auto_content_aspect_ratio(&self) -> bool {
        self.replaced_content().auto_content_aspect_ratio_denominator != crate::layout::CssPixels::default()
    }

    pub(crate) fn auto_content_aspect_ratio_numerator(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_aspect_ratio_numerator
    }

    pub(crate) fn auto_content_aspect_ratio_denominator(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_aspect_ratio_denominator
    }

    pub(crate) fn has_auto_content_box_size(&self) -> bool {
        crate::layout::node_has_auto_content_box_size(self.data())
    }

    fn node_has_size_containment(&self) -> bool {
        if !matches!(
            self.data().table_display,
            FfiTableDisplay::Other | FfiTableDisplay::TableCaption
        ) {
            return false;
        }
        let containment_bits = self.style().containment_bits();
        containment_bits & 1 != 0 || containment_bits & (1 << 5) != 0
    }

    pub(crate) fn has_preferred_aspect_ratio(&self) -> bool {
        self.preferred_aspect_ratio().is_some()
    }

    pub(crate) fn preferred_aspect_ratio(&self) -> Option<PixelFraction> {
        let style = self.style();
        if !self.node_has_size_containment() && style.aspect_ratio_uses_natural_when_available() {
            let replaced = self.replaced_content();
            if replaced.auto_content_aspect_ratio_denominator != crate::layout::CssPixels::default() {
                return Some(PixelFraction {
                    numerator: replaced.auto_content_aspect_ratio_numerator,
                    denominator: replaced.auto_content_aspect_ratio_denominator,
                });
            }
        }
        let denominator = style.css_preferred_aspect_ratio_denominator();
        (denominator != crate::layout::CssPixels::default()).then(|| PixelFraction {
            numerator: style.css_preferred_aspect_ratio_numerator(),
            denominator,
        })
    }

    pub(crate) fn has_default_preferred_width(&self) -> bool {
        self.replaced_content().has_default_preferred_width
    }

    pub(crate) fn default_preferred_width(&self) -> crate::layout::CssPixels {
        self.replaced_content().default_preferred_width
    }

    pub(crate) fn has_default_preferred_height(&self) -> bool {
        self.replaced_content().has_default_preferred_height
    }

    pub(crate) fn default_preferred_height(&self) -> crate::layout::CssPixels {
        self.replaced_content().default_preferred_height
    }

    pub(crate) fn initial_containing_block_inline_size(&self) -> crate::layout::CssPixels {
        self.callbacks.initial_containing_block_inline_size
    }

    pub(crate) fn is_scroll_container(&self) -> bool {
        if self.data().kind == NodeKind::Viewport {
            return true;
        }
        let style = self.style();
        let overflow_value_makes_box_a_scroll_container =
            |overflow: u8| matches!(overflow, overflow::AUTO | overflow::HIDDEN | overflow::SCROLL);
        overflow_value_makes_box_a_scroll_container(style.overflow_x)
            || overflow_value_makes_box_a_scroll_container(style.overflow_y)
    }

    pub(crate) fn display(&self) -> crate::layout::FfiDisplay {
        if self.data().style.is_null() {
            return crate::layout::FfiDisplay::block();
        }
        self.state.style_snapshot(self.callbacks, self.node).display
    }

    pub(crate) fn is_svg_box(&self) -> bool {
        crate::layout::kind_is_svg_box(self.data().kind)
    }

    pub(crate) fn is_svg_svg_box(&self) -> bool {
        self.data().kind == NodeKind::SVGSVGBox
    }

    pub(crate) fn is_table_box(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableRoot
    }

    pub(crate) fn is_table_wrapper(&self) -> bool {
        self.data().kind == NodeKind::TableWrapper
    }

    pub(crate) fn is_table_row_group(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableRowGroup
    }

    pub(crate) fn is_table_header_group(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableHeaderGroup
    }

    pub(crate) fn is_table_footer_group(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableFooterGroup
    }

    pub(crate) fn is_table_row(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableRow
    }

    pub(crate) fn is_table_cell(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableCell
    }

    pub(crate) fn is_table_column_group(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableColumnGroup
    }

    pub(crate) fn is_table_column(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableColumn
    }

    pub(crate) fn is_table_caption(&self) -> bool {
        self.data().table_display == FfiTableDisplay::TableCaption
    }

    pub(crate) fn is_viewport(&self) -> bool {
        self.data().kind == NodeKind::Viewport
    }

    pub(crate) fn document_in_quirks_mode(&self) -> bool {
        self.callbacks.document_in_quirks_mode
    }

    pub(crate) fn is_in_user_agent_shadow_tree(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsInUserAgentShadowTree)
    }

    pub(crate) fn is_html_html_element(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsHtmlHtmlElement)
    }

    pub(crate) fn is_html_body_element(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsBody)
    }
}

pub(crate) struct LayoutState {
    used_values: PagedStore<UsedValues>,
    contained_abspos_children: PagedStore<RefCell<Vec<ContainedAbsposChild>>>,
    style_decode_values: PagedStore<StyleDecodeValues>,
    style_snapshots: PagedStore<FfiStyleSnapshot>,
    replaced_content_facts: PagedStore<crate::layout::FfiReplacedContentFacts>,
    list_item_facts: PagedStore<crate::layout::FfiListItemFacts>,
    table_facts: PagedStore<FfiTableBoxFacts>,
    grid_facts: PagedStore<GridStyleFacts>,
    line_data: PagedStore<RefCell<LineData>>,
    block_rare_data: PagedStore<RefCell<BlockRareData>>,
    used_values_rare_data: PagedStore<RefCell<UsedValuesRareData>>,
    precreated_used_value_slots: RefCell<HashSet<u32>>,
    purpose: LayoutStatePurpose,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum LayoutStatePurpose {
    Commit,
    Measurement,
}

impl Default for LayoutState {
    fn default() -> Self {
        Self::new(LayoutStatePurpose::Commit)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct BlockRareData {
    pub(crate) lowest_floating_descendant_bottom_margin_edge: Option<crate::layout::CssPixels>,
}

#[derive(Default)]
pub(crate) struct LineData {
    pub(crate) line_boxes: Vec<LineBoxData>,
    pub(crate) inline_box_pieces: Vec<InlineBoxPieceData>,
}

#[derive(Default)]
pub(crate) struct UsedValuesRareData {
    pub(crate) table_cell_coordinates: Option<FfiTableCellCoordinates>,
    pub(crate) computed_svg_path: Option<RetainedLayoutHandle>,
    pub(crate) computed_svg_transforms: Option<crate::layout::FfiSvgComputedTransforms>,
    pub(crate) grid_layout_data: Option<OwnedGridLayoutData>,
    pub(crate) flex_layout_data: Option<OwnedFlexLayoutData>,
    pub(crate) used_grid_tracks: Option<OwnedUsedGridTracks>,
    pub(crate) override_borders_data: Option<FfiBordersData>,
    pub(crate) abspos_layout_inputs: Option<AbsposLayoutInputs>,
}

impl LayoutState {
    pub(crate) fn new(purpose: LayoutStatePurpose) -> Self {
        Self {
            used_values: PagedStore::default(),
            contained_abspos_children: PagedStore::default(),
            style_decode_values: PagedStore::default(),
            style_snapshots: PagedStore::default(),
            replaced_content_facts: PagedStore::default(),
            list_item_facts: PagedStore::default(),
            table_facts: PagedStore::default(),
            grid_facts: PagedStore::default(),
            line_data: PagedStore::default(),
            block_rare_data: PagedStore::default(),
            used_values_rare_data: PagedStore::default(),
            precreated_used_value_slots: RefCell::new(HashSet::new()),
            purpose,
        }
    }

    pub(crate) fn is_measurement(&self) -> bool {
        self.purpose == LayoutStatePurpose::Measurement
    }

    pub(crate) fn record_precreated_used_values(&self, callbacks: &FfiLayoutFcCallbacks, node: Node) {
        assert!(!self.is_measurement());
        let slot_index = callbacks.slot_index(node);
        assert!(self.used_values.get(slot_index).is_some());
        assert!(self.precreated_used_value_slots.borrow_mut().insert(slot_index));
    }

    #[inline]
    pub(crate) fn node_facts<'pass>(
        &'pass self,
        callbacks: &'pass FfiLayoutFcCallbacks,
        node: Node,
    ) -> NodeFacts<'pass> {
        NodeFacts {
            state: self,
            callbacks,
            node,
        }
    }

    pub(crate) fn may_reuse_precreated_used_values(&self, slot_index: u32) -> bool {
        self.precreated_used_value_slots.borrow().contains(&slot_index)
    }

    pub(crate) fn create_used_values(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &UsedValues {
        assert!(!node.is_invalid());
        let facts = self.node_facts(callbacks, node);
        let slot_index = callbacks.slot_index(node);
        assert!(self.used_values.get(slot_index).is_none());

        let style = self.style_facts(callbacks, node);
        let percentage_basis_inline_size = constraints.percentage_basis_inline_size;
        let percentage_basis_block_size = constraints.percentage_basis_block_size;

        // NOTE: In the code below, we decide if `node` has definite inline
        // and/or block size. This attempts to cover all the *general* cases
        // where CSS considers sizes to be definite. If `node` has definite
        // values for min/max-width or min/max-height and a definite preferred
        // size in the same axis, we clamp the preferred size here as well.
        //
        // There are additional cases where CSS considers values to be
        // definite. We model all of those by considering sizes definite once
        // they are assigned through set_content_inline_size() or
        // set_content_block_size().
        let used = UsedValues {
            node,
            ..UsedValues::default()
        };

        #[derive(Clone, Copy)]
        enum Axis {
            Inline,
            Block,
        }

        let containing_block_size_for_axis = |axis: Axis| match axis {
            Axis::Inline => percentage_basis_inline_size.unwrap_or_default(),
            Axis::Block => percentage_basis_block_size.unwrap_or_default(),
        };
        let containing_block_has_definite_size = |axis: Axis| match axis {
            Axis::Inline => percentage_basis_inline_size.is_some(),
            Axis::Block => percentage_basis_block_size.is_some(),
        };

        let adjust_for_box_sizing = |unadjusted: crate::layout::CssPixels, computed_size: crate::layout::FfiSizeValue, axis: Axis| {
            // box-sizing: content-box and automatic sizes need no
            // adjustment.
            if style.box_sizing == box_sizing::CONTENT_BOX || computed_size.is_auto() {
                return unadjusted;
            }

            // box-sizing: border-box subtracts the relevant border and
            // padding. Block-axis padding percentages also resolve against
            // the containing block's inline size.
            let inline_basis = percentage_basis_inline_size.unwrap_or_default();
            let border_and_padding = match axis {
                Axis::Inline => {
                    style.border_left_width
                        + style.padding_left().to_px(inline_basis)
                        + style.border_right_width
                        + style.padding_right().to_px(inline_basis)
                }
                Axis::Block => {
                    style.border_top_width
                        + style.padding_top().to_px(inline_basis)
                        + style.border_bottom_width
                        + style.padding_bottom().to_px(inline_basis)
                }
            };
            unadjusted - border_and_padding
        };

        let parent = callbacks.parent(node);
        let parent_facts = (!parent.is_invalid()).then(|| self.node_facts(callbacks, parent));
        let is_definite_size = |size: crate::layout::FfiSizeValue, axis: Axis| -> Option<crate::layout::CssPixels> {
            // A definite size can be determined without performing
            // layout: a length, an initial-containing-block size, or a
            // percentage/formula resolved solely against definite sizes.
            if size.is_auto() {
                // The inline size of a non-flex-item block is definite when
                // it is auto and its containing block has a definite inline
                // size. This is the stretch-fit case from css-sizing-3.
                // Replaced boxes remain content-based until layout.
                if matches!(axis, Axis::Inline)
                    && !facts.is_replaced_box()
                    && !facts.is_floating()
                    && !facts.is_absolutely_positioned()
                    && facts.display().is_block_outside()
                    && parent_facts.is_some_and(|parent| {
                        !parent.is_floating()
                            && (parent.display().is_flow_root_inside() || parent.display().is_flow_inside())
                    })
                    && containing_block_has_definite_size(Axis::Inline)
                {
                    let available = containing_block_size_for_axis(Axis::Inline);
                    return Some(clamp_to_max_dimension_value(
                        available
                            - used.margin_left.get()
                            - used.margin_right.get()
                            - used.padding_left.get()
                            - used.padding_right.get()
                            - used.border_left.get()
                            - used.border_right.get(),
                    ));
                }
                return None;
            }

            if !size.is_length_percentage() {
                return None;
            }
            if size.contains_percentage && !containing_block_has_definite_size(axis) {
                return None;
            }
            let basis = if size.contains_percentage {
                containing_block_size_for_axis(axis)
            } else {
                crate::layout::CssPixels::default()
            };
            Some(clamp_to_max_dimension_value(adjust_for_box_sizing(
                size.to_px(basis),
                size,
                axis,
            )))
        };

        let min_inline_size = is_definite_size(style.min_width(), Axis::Inline);
        let max_inline_size = is_definite_size(style.max_width(), Axis::Inline);
        let min_block_size = is_definite_size(style.min_height(), Axis::Block);
        let max_block_size = is_definite_size(style.max_height(), Axis::Block);
        let mut content_inline_size = is_definite_size(style.width(), Axis::Inline);
        let mut content_block_size = is_definite_size(style.height(), Axis::Block);

        used.has_definite_inline_size.set(content_inline_size.is_some());
        used.has_definite_block_size.set(content_block_size.is_some());
        if let Some(size) = content_inline_size.as_mut() {
            if let Some(minimum) = min_inline_size {
                *size = clamp_to_max_dimension_value((*size).max(minimum));
            }
            if let Some(maximum) = max_inline_size {
                *size = clamp_to_max_dimension_value((*size).min(maximum));
            }
        }
        if let Some(size) = content_block_size.as_mut() {
            if let Some(minimum) = min_block_size {
                *size = clamp_to_max_dimension_value((*size).max(minimum));
            }
            if let Some(maximum) = max_block_size {
                *size = clamp_to_max_dimension_value((*size).min(maximum));
            }
        }
        used.content_inline_size.set(content_inline_size.unwrap_or_default());
        used.content_block_size.set(content_block_size.unwrap_or_default());

        let used = self.used_values.allocate(slot_index, used);

        let list_item_marker = facts.is_list_item_box().then(|| facts.list_item_marker());
        if let Some(list_item_marker) = list_item_marker.filter(|marker| !marker.is_invalid()) {
            let marker_slot_index = callbacks.slot_index(list_item_marker);
            if self.used_values.get(marker_slot_index).is_none() {
                // List markers inherit only bases that were already definite
                // when their list item entry was created.
                let marker_constraints = ContainingBlockConstraints {
                    percentage_basis_inline_size: used
                        .has_definite_inline_size()
                        .then_some(used.content_inline_size.get()),
                    percentage_basis_block_size: used
                        .has_definite_block_size()
                        .then_some(used.content_block_size.get()),
                    ..ContainingBlockConstraints::default()
                };
                self.create_used_values(callbacks, list_item_marker, marker_constraints);
            }
        }

        used
    }

    pub(crate) fn populate_from_paintable(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        paintable: *mut c_void,
    ) -> Option<&UsedValues> {
        let slot_index = callbacks.slot_index(node);
        assert!(self.used_values.get(slot_index).is_none());

        let mut geometry = FfiPaintableGeometry::default();
        let found =
            unsafe {
                (callbacks.read_paintable_geometry)(callbacks.context, callbacks.shell(node), paintable, &raw mut geometry)
            };
        if !found {
            return None;
        }

        // Skip normal node initialization: resolving computed sizes requires
        // percentage bases, and every resulting geometry field is replaced by
        // the previous paintable's committed value immediately.
        let used = UsedValues {
            node,
            ..UsedValues::default()
        };
        used.materialized_from_paintable.set(true);
        used.set_content_inline_size(geometry.content_inline_size);
        used.set_content_block_size(geometry.content_block_size);
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);
        used.has_content_offset.set(true);
        used.content_offset.set(geometry.content_offset);
        used.margin_left.set(geometry.margin_left);
        used.margin_right.set(geometry.margin_right);
        used.margin_top.set(geometry.margin_top);
        used.margin_bottom.set(geometry.margin_bottom);
        used.border_left.set(geometry.border_left);
        used.border_right.set(geometry.border_right);
        used.border_top.set(geometry.border_top);
        used.border_bottom.set(geometry.border_bottom);
        used.padding_left.set(geometry.padding_left);
        used.padding_right.set(geometry.padding_right);
        used.padding_top.set(geometry.padding_top);
        used.padding_bottom.set(geometry.padding_bottom);
        used.inset_left.set(geometry.inset_left);
        used.inset_right.set(geometry.inset_right);
        used.inset_top.set(geometry.inset_top);
        used.inset_bottom.set(geometry.inset_bottom);

        Some(self.used_values.allocate(slot_index, used))
    }

    pub(crate) fn used_value_nodes(&self) -> Vec<Node> {
        let mut nodes = Vec::new();
        self.used_values.for_each(|used| nodes.push(used.node));
        nodes
    }

    pub(crate) fn set_box_is_grid_item(&self, callbacks: &FfiLayoutFcCallbacks, node: Node, is_grid_item: bool) {
        callbacks.arena().set_node_flag(node, NodeFlag::IsGridItem, is_grid_item);
    }

    pub(crate) fn set_box_is_flex_item(&self, callbacks: &FfiLayoutFcCallbacks, node: Node, is_flex_item: bool) {
        callbacks.arena().set_node_flag(node, NodeFlag::IsFlexItem, is_flex_item);
    }

    pub(crate) fn style_facts<'pass>(
        &'pass self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
    ) -> StyleValues<'pass> {
        let slot_index = callbacks.slot_index(node);
        let cache = if let Some(cache) = self.style_decode_values.get(slot_index) {
            cache
        } else {
            let snapshot = self.style_snapshot(callbacks, node);
            self.style_decode_values.allocate(
                slot_index,
                StyleDecodeValues::new(
                    snapshot.payloads,
                    snapshot.display,
                    callbacks.release_calc_handle,
                    callbacks.release_anchor_name_handle,
                ),
            )
        };
        StyleValues::new(cache, callbacks.context, callbacks.decode_residual_style)
    }

    fn style_snapshot(&self, callbacks: &FfiLayoutFcCallbacks, node: Node) -> &FfiStyleSnapshot {
        let data = callbacks.node_data(node);
        assert!(!data.style.is_null());
        let slot_index = callbacks.slot_index(node);
        let snapshot = self.style_snapshots.get(slot_index);
        if let Some(snapshot) = snapshot {
            return snapshot;
        }
        // SAFETY: NodeData retains the node's immutable ComputedValues for the
        // synchronous layout pass.
        let snapshot = unsafe { (callbacks.build_style_snapshot)(callbacks.context, data.style) };
        self.style_snapshots.allocate(slot_index, snapshot)
    }

    pub(crate) fn replace_resolved_anchor_insets(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        resolved: crate::layout::FfiResolvedAnchorInsets,
    ) {
        let slot_index = callbacks.slot_index(node);
        let cache = self
            .style_decode_values
            .get(slot_index)
            .expect("missing style decode cache");
        let replace = |field: SizeField, is_auto: bool, value: crate::layout::CssPixels| {
            let value = if is_auto {
                crate::layout::FfiSizeValue::auto_value()
            } else {
                crate::layout::FfiSizeValue::px_value(value)
            };
            cache.replace_size(field, value);
        };
        if resolved.resolves_top {
            replace(SizeField::InsetTop, resolved.top_is_auto, resolved.top);
        }
        if resolved.resolves_right {
            replace(SizeField::InsetRight, resolved.right_is_auto, resolved.right);
        }
        if resolved.resolves_bottom {
            replace(SizeField::InsetBottom, resolved.bottom_is_auto, resolved.bottom);
        }
        if resolved.resolves_left {
            replace(SizeField::InsetLeft, resolved.left_is_auto, resolved.left);
        }
    }

    pub(crate) fn replaced_content_facts(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
    ) -> crate::layout::FfiReplacedContentFacts {
        let slot_index = callbacks.slot_index(node);
        if let Some(facts) = self.replaced_content_facts.get(slot_index) {
            return *facts;
        }
        let data = callbacks.node_data(node);
        // Size containment gives any box an auto content box size of zero, so
        // size-contained boxes join the replaced kinds in fetching real facts.
        let size_containment_may_apply = crate::layout::kind_is_box(data.kind) && !data.style.is_null() && {
            let containment_bits = self.style_facts(callbacks, node).containment_bits();
            containment_bits & 1 != 0 || containment_bits & (1 << 5) != 0
        };
        let facts = if crate::layout::node_may_have_replaced_content_facts(data) || size_containment_may_apply {
            // SAFETY: The callback table and node are supplied by the live C++
            // formatting-context shim and remain valid for this layout pass.
            unsafe { (callbacks.build_replaced_content_facts)(callbacks.context, callbacks.shell(node)) }
        } else {
            crate::layout::FfiReplacedContentFacts::default()
        };
        self.replaced_content_facts.allocate(slot_index, facts);
        facts
    }

    pub(crate) fn list_item_facts(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
    ) -> crate::layout::FfiListItemFacts {
        let slot_index = callbacks.slot_index(node);
        if let Some(facts) = self.list_item_facts.get(slot_index) {
            return *facts;
        }
        let facts = if crate::layout::node_may_have_list_item_facts(callbacks.node_data(node)) {
            // SAFETY: The callback table and node are supplied by the live C++
            // formatting-context shim and remain valid for this layout pass.
            unsafe { (callbacks.build_list_item_facts)(callbacks.context, callbacks.shell(node)) }
        } else {
            crate::layout::FfiListItemFacts::default()
        };
        self.list_item_facts.allocate(slot_index, facts);
        facts
    }

    pub(crate) fn table_facts(&self, callbacks: &FfiLayoutFcCallbacks, node: Node) -> FfiTableBoxFacts {
        let slot_index = callbacks.slot_index(node);
        let facts = self.table_facts.get(slot_index);
        if let Some(facts) = facts {
            return *facts;
        }
        // SAFETY: The callback table and node are supplied by the live C++
        // formatting-context shim and remain valid for this layout pass.
        let facts = unsafe { (callbacks.build_table_box_facts)(callbacks.context, callbacks.shell(node)) };
        self.table_facts.allocate(slot_index, facts);
        facts
    }

    pub(crate) fn grid_facts(&self, callbacks: &FfiLayoutFcCallbacks, node: Node) -> &GridStyleFacts {
        let slot_index = callbacks.slot_index(node);
        let facts = self.grid_facts.get(slot_index);
        if let Some(facts) = facts {
            return facts;
        }
        let facts = GridStyleFacts::build(callbacks, node);
        self.grid_facts.allocate(slot_index, facts)
    }

    pub(crate) fn text_chunks(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        should_wrap_lines: bool,
        should_respect_linebreaks: bool,
        unidirectional_ltr: bool,
    ) -> &'static [TextChunk] {
        let parent_style = self.style_facts(callbacks, callbacks.parent(node));
        let key = crate::layout::layout_node_arena::TextChunkCacheKey {
            should_wrap_lines,
            should_respect_linebreaks,
            unidirectional_ltr,
            white_space_collapse: parent_style.white_space_collapse,
            word_break: parent_style.word_break,
            font_variant_emoji: parent_style.font_variant_emoji,
            font_cascade_list: parent_style.font_cascade_list(),
        };
        let text = &callbacks.text_content(node).text;
        callbacks.arena().text_chunks(node, key, || {
            chunk_text(TextChunkInputs {
                text,
                font_cascade_list: key.font_cascade_list,
                white_space_collapse: key.white_space_collapse,
                word_break: key.word_break,
                font_variant_emoji: key.font_variant_emoji,
                should_wrap_lines,
                should_respect_linebreaks,
                unidirectional_ltr,
            })
        })
    }

    pub(crate) fn line_data_cell(&self, slot_index: u32) -> &RefCell<LineData> {
        self.line_data
            .get(slot_index)
            .unwrap_or_else(|| self.line_data.allocate(slot_index, RefCell::new(LineData::default())))
    }

    pub(crate) fn line_data(&self, slot_index: u32) -> Option<Ref<'_, LineData>> {
        self.line_data.get(slot_index).map(RefCell::borrow)
    }

    pub(crate) fn line_data_mut_if_present(&self, slot_index: u32) -> Option<RefMut<'_, LineData>> {
        self.line_data.get(slot_index).map(RefCell::borrow_mut)
    }

    pub(crate) fn block_rare_data(&self, slot_index: u32) -> Option<Ref<'_, BlockRareData>> {
        self.block_rare_data.get(slot_index).map(RefCell::borrow)
    }

    pub(crate) fn block_rare_data_mut(&self, slot_index: u32) -> RefMut<'_, BlockRareData> {
        self.block_rare_data
            .get(slot_index)
            .unwrap_or_else(|| {
                self.block_rare_data
                    .allocate(slot_index, RefCell::new(BlockRareData::default()))
            })
            .borrow_mut()
    }

    pub(crate) fn used_values_rare_data(&self, slot_index: u32) -> Option<Ref<'_, UsedValuesRareData>> {
        self.used_values_rare_data.get(slot_index).map(RefCell::borrow)
    }

    pub(crate) fn used_values_rare_data_mut(&self, slot_index: u32) -> RefMut<'_, UsedValuesRareData> {
        self.used_values_rare_data
            .get(slot_index)
            .unwrap_or_else(|| {
                self.used_values_rare_data
                    .allocate(slot_index, RefCell::new(UsedValuesRareData::default()))
            })
            .borrow_mut()
    }

    pub(crate) fn used_values_rare_data_for_node_mut(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
    ) -> RefMut<'_, UsedValuesRareData> {
        self.used_values_rare_data_mut(callbacks.slot_index(node))
    }

    pub(crate) fn used_values_by_slot(&self, slot_index: u32) -> Option<&UsedValues> {
        self.used_values.get(slot_index)
    }

    pub(crate) fn used_values(&self, callbacks: &FfiLayoutFcCallbacks, node: Node) -> &UsedValues {
        self.used_values
            .get(callbacks.slot_index(node))
            .expect("missing used values")
    }

    pub(crate) fn try_used_values(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
    ) -> Option<&UsedValues> {
        self.used_values.get(callbacks.slot_index(node))
    }

    pub(crate) fn register_contained_abspos_child(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        target_box: Node,
        child_box: Node,
        static_position_rect: StaticPositionRect,
    ) {
        let slot_index = target_box.slot_index();
        let children = self.contained_abspos_children.get(slot_index).unwrap_or_else(|| {
            self.contained_abspos_children
                .allocate(slot_index, RefCell::new(Vec::new()))
        });
        let mut children = children.borrow_mut();
        // Entries are inserted in tree order so consumption follows document order.
        let mut insertion_index = children.len();
        for (index, entry) in children.iter().enumerate() {
            // Every box is laid out at most once per state, so it can only be registered once.
            assert_ne!(entry.child_box, child_box);
            if insertion_index == children.len() && callbacks.is_before(child_box, entry.child_box) {
                insertion_index = index;
            }
        }
        children.insert(
            insertion_index,
            ContainedAbsposChild {
                child_box,
                static_position_rect,
            },
        );
    }

    pub(crate) fn take_next_contained_abspos_child(&self, target_box: Node) -> Option<PendingAbsposChild> {
        let children = self.contained_abspos_children.get(target_box.slot_index())?;
        let mut children = children.borrow_mut();
        if children.is_empty() {
            return None;
        }
        let child = children.remove(0);
        Some(PendingAbsposChild {
            child_box: child.child_box,
            static_position_rect: child.static_position_rect,
        })
    }

    fn line_fragment_lookup(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        used: &UsedValues,
    ) -> (FfiLineFragmentLookup, crate::layout::FfiCssPixelPoint) {
        if !used.has_containing_line_box_fragment.get() {
            return (FfiLineFragmentLookup::NotFound, crate::layout::FfiCssPixelPoint::default());
        }
        // SAFETY: Commit traverses the still-live C++ layout tree
        // synchronously with the pass callback table.
        let containing_block = callbacks.containing_block(used.node);
        assert!(!containing_block.is_invalid());
        let slot_index = callbacks.slot_index(containing_block);
        let Some(data) = self.line_data(slot_index) else {
            return (FfiLineFragmentLookup::NotFound, crate::layout::FfiCssPixelPoint::default());
        };
        let coordinate = used.containing_line_box_fragment.get();
        let Some(line) = data.line_boxes.get(coordinate.line_box_index) else {
            return (FfiLineFragmentLookup::NotFound, crate::layout::FfiCssPixelPoint::default());
        };
        let Some(fragment) = line.fragments.get(coordinate.fragment_index) else {
            return (FfiLineFragmentLookup::LineOnly, crate::layout::FfiCssPixelPoint::default());
        };
        let (x, y) = fragment.offset();
        (FfiLineFragmentLookup::Found, crate::layout::FfiCssPixelPoint { x, y })
    }

    fn commit_subtree(
        &self,
        node: Node,
        parent_paintable: *mut c_void,
        insert_before_paintable: *mut c_void,
        callbacks: &FfiLayoutFcCallbacks,
        sink: &FfiCommitSink,
    ) {
        let slot_index = callbacks.slot_index(node);
        let used = self.used_values_by_slot(slot_index);
        let abspos_layout_inputs = self
            .used_values_rare_data(slot_index)
            .and_then(|rare| rare.abspos_layout_inputs);
        if used.is_some() {
            callbacks.set_saved_abspos_layout_inputs(node, abspos_layout_inputs);
        }
        // SAFETY: The C++ sink owns paintables and copies every plain-data
        // input synchronously.
        let node_shell = callbacks.shell(node);
        let paintable = unsafe { (sink.prepare_node)(sink.context, node_shell, used.is_some()) };

        if let Some(used) = used
            && !paintable.is_null()
        {
            // SAFETY: Every callback below copies its plain-data argument or
            // consumes one retained handle synchronously.
            unsafe {
                (sink.set_box_metrics)(
                    sink.context,
                    paintable,
                    FfiCommittedBoxMetrics {
                        content_inline_size: used.content_inline_size.get(),
                        content_block_size: used.content_block_size.get(),
                        margin_left: used.margin_left.get(),
                        margin_right: used.margin_right.get(),
                        margin_top: used.margin_top.get(),
                        margin_bottom: used.margin_bottom.get(),
                        border_left: used.border_left.get(),
                        border_right: used.border_right.get(),
                        border_top: used.border_top.get(),
                        border_bottom: used.border_bottom.get(),
                        padding_left: used.padding_left.get(),
                        padding_right: used.padding_right.get(),
                        padding_top: used.padding_top.get(),
                        padding_bottom: used.padding_bottom.get(),
                        inset_left: used.inset_left.get(),
                        inset_right: used.inset_right.get(),
                        inset_top: used.inset_top.get(),
                        inset_bottom: used.inset_bottom.get(),
                    },
                );
            }

            let (line_fragment_lookup, line_fragment_offset) = self.line_fragment_lookup(callbacks, used);
            let committed_offset = FfiCommittedOffset {
                offset: used.content_offset.get(),
                inset_left: used.inset_left.get(),
                inset_top: used.inset_top.get(),
                materialized_from_paintable: used.materialized_from_paintable.get(),
                has_containing_line_box_fragment: used.has_containing_line_box_fragment.get(),
                containing_line_box_index: used.containing_line_box_fragment.get().line_box_index,
                line_fragment_lookup,
                line_fragment_offset,
            };

            let (override_borders, table_cell_coordinates) =
                self.used_values_rare_data(slot_index).map_or((None, None), |rare| {
                    (rare.override_borders_data, rare.table_cell_coordinates)
                });
            unsafe {
                if let Some(borders) = override_borders {
                    (sink.set_override_borders)(sink.context, paintable, borders);
                }
                if let Some(coordinates) = table_cell_coordinates {
                    (sink.set_table_cell_coordinates)(sink.context, paintable, coordinates);
                }
            }

            let has_line_data = self.line_data(slot_index).is_some();
            if has_line_data {
                // SAFETY: The sink keeps one line accumulator live between
                // begin_line_data() and finish_line_data().
                let accepts_lines = unsafe { (sink.begin_line_data)(sink.context, paintable) };
                if accepts_lines {
                    let line_sink = FfiLineSinkCallbacks {
                        context: sink.context,
                        begin_line: sink.begin_line,
                        emit_fragment: sink.emit_fragment,
                        emit_inline_box_piece: sink.emit_inline_box_piece,
                    };
                    assert!(push_line_data(self, slot_index, callbacks, line_sink));
                    unsafe {
                        (sink.finish_line_data)(sink.context);
                    }
                }
            }

            let (transforms, path) = self
                .used_values_rare_data_mut_if_present(slot_index)
                .map_or((None, None), |mut rare| {
                    (
                        rare.computed_svg_transforms,
                        rare.computed_svg_path.as_mut().map(RetainedLayoutHandle::take),
                    )
                });
            unsafe {
                if let Some(transforms) = transforms {
                    (sink.set_computed_svg_transforms)(sink.context, paintable, transforms);
                }
                if let Some(path) = path {
                    (sink.set_computed_svg_path)(sink.context, paintable, path);
                }
            }
            if let Some(rare) = self.used_values_rare_data(slot_index) {
                if let Some(data) = &rare.grid_layout_data {
                    data.with_ffi_view(|view| {
                        // SAFETY: The Rust-owned nested vectors remain live
                        // while the commit sink copies this borrowed view.
                        unsafe { (sink.set_grid_layout_data)(sink.context, paintable, view) };
                    });
                }
                if let Some(data) = &rare.flex_layout_data {
                    data.with_ffi_view(|view| {
                        // SAFETY: The Rust-owned lines and items remain live
                        // while the commit sink copies this borrowed view.
                        unsafe { (sink.set_flex_layout_data)(sink.context, paintable, view) };
                    });
                }
                if let Some(tracks) = &rare.used_grid_tracks {
                    tracks.with_ffi_views(|columns, rows| {
                        // SAFETY: Both Rust-owned track lists remain live
                        // while the commit sink copies these borrowed views.
                        unsafe { (sink.set_used_grid_tracks)(sink.context, paintable, columns, rows) };
                    });
                }
            }
            unsafe {
                (sink.set_offset)(sink.context, node_shell, paintable, committed_offset);
            }
        }

        // SAFETY: Wiring uses only live layout and paintable pointers for this
        // synchronous commit.
        let result = unsafe {
            (sink.finish_node)(
                sink.context,
                node_shell,
                paintable,
                parent_paintable,
                insert_before_paintable,
            )
        };
        assert_eq!(result.paintable, paintable);

        let mut child = callbacks.first_child(node);
        while !child.is_invalid() {
            let next = callbacks.next_sibling(child);
            self.commit_subtree(child, result.paintable_for_children, null_mut(), callbacks, sink);
            child = next;
        }
    }

    pub(crate) fn commit_at(
        &self,
        root: Node,
        parent_paintable: *mut c_void,
        insert_before_paintable: *mut c_void,
        callbacks: &FfiLayoutFcCallbacks,
        sink: &FfiCommitSink,
    ) {
        self.commit_subtree(root, parent_paintable, insert_before_paintable, callbacks, sink);

        // Relative inline ancestor offsets require the complete paint tree.
        // Keep the pass paintable-only on the C++ side, but preserve the used
        // values store's original page/slot iteration order.
        self.used_values.for_each(|used| unsafe {
            (sink.resolve_relative_positions)(sink.context, callbacks.shell(used.node));
        });
    }

    pub(crate) fn commit_replacing(
        &self,
        root: Node,
        paintable_to_replace: *mut c_void,
        callbacks: &FfiLayoutFcCallbacks,
        sink: &FfiCommitSink,
    ) {
        // SAFETY: The sink retains the replaced paintable, detaches it, and
        // returns borrowed insertion pointers that stay live until
        // finish_commit().
        let position = unsafe { (sink.begin_commit)(sink.context, callbacks.shell(root), paintable_to_replace) };
        self.commit_at(
            root,
            position.parent_paintable,
            position.insert_before_paintable,
            callbacks,
            sink,
        );
        unsafe {
            (sink.finish_commit)(sink.context);
        }
    }
}

impl LayoutState {
    fn used_values_rare_data_mut_if_present(&self, slot_index: u32) -> Option<RefMut<'_, UsedValuesRareData>> {
        self.used_values_rare_data.get(slot_index).map(RefCell::borrow_mut)
    }
}
