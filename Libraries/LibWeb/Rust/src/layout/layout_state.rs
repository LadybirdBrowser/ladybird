/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const PAGE_BITS: usize = 4;
const PAGE_SIZE: usize = 1 << PAGE_BITS;
const PAGE_MASK: usize = PAGE_SIZE - 1;
const PAGE_TABLE_BITS: usize = 10;
const PAGE_TABLE_FANOUT: usize = 1 << PAGE_TABLE_BITS;
const PAGE_TABLE_MASK: usize = PAGE_TABLE_FANOUT - 1;
const ADDRESSABLE_SLOT_INDEX_COUNT: usize = 1 << (2 * PAGE_TABLE_BITS + PAGE_BITS);

type Page<T> = [OnceCell<T>; PAGE_SIZE];
type PageTable<T> = [OnceCell<Box<Page<T>>>; PAGE_TABLE_FANOUT];
type PageTableDirectory<T> = [OnceCell<Box<PageTable<T>>>; PAGE_TABLE_FANOUT];

pub(crate) struct PagedStore<T> {
    page_table_directory: OnceCell<Box<PageTableDirectory<T>>>,
}

impl<T> Default for PagedStore<T> {
    fn default() -> Self {
        Self {
            page_table_directory: OnceCell::new(),
        }
    }
}

impl<T> PagedStore<T> {
    fn empty_level<Entry, const FANOUT: usize>() -> Box<[OnceCell<Entry>; FANOUT]> {
        Box::new([const { OnceCell::new() }; FANOUT])
    }

    fn split_index(index: u32) -> (usize, usize, usize) {
        let index = index as usize;
        (
            index >> (PAGE_TABLE_BITS + PAGE_BITS),
            (index >> PAGE_BITS) & PAGE_TABLE_MASK,
            index & PAGE_MASK,
        )
    }

    #[inline]
    pub(crate) fn get(&self, index: u32) -> Option<&T> {
        let (directory_index, page_table_index, entry_index) = Self::split_index(index);
        self.page_table_directory
            .get()?
            .get(directory_index)?
            .get()?[page_table_index]
            .get()?[entry_index]
            .get()
    }

    pub(crate) fn allocate(&self, index: u32, value: T) -> &T {
        assert!((index as usize) < ADDRESSABLE_SLOT_INDEX_COUNT);
        let (directory_index, page_table_index, entry_index) = Self::split_index(index);
        let page_table =
            self.page_table_directory.get_or_init(Self::empty_level)[directory_index].get_or_init(Self::empty_level);
        let entry = &page_table[page_table_index].get_or_init(Self::empty_level)[entry_index];
        let entry_was_vacant = entry.set(value).is_ok();
        assert!(entry_was_vacant, "PagedStore index {index} allocated twice");
        entry.get().unwrap()
    }

    fn for_each_indexed(&self, mut callback: impl FnMut(u32, &T)) {
        let Some(directory) = self.page_table_directory.get() else {
            return;
        };
        for (directory_index, page_table) in directory.iter().enumerate() {
            let Some(page_table) = page_table.get() else {
                continue;
            };
            for (page_index, page) in page_table.iter().enumerate() {
                let Some(page) = page.get() else {
                    continue;
                };
                for (entry_index, entry) in page.iter().enumerate() {
                    if let Some(entry) = entry.get() {
                        let index = (directory_index << (PAGE_TABLE_BITS + PAGE_BITS))
                            | (page_index << PAGE_BITS)
                            | entry_index;
                        callback(index as u32, entry);
                    }
                }
            }
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
    pub content_offset: crate::layout::FfiCssPixelPoint,
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
    pub containing_line_box_index: usize,
    pub has_containing_line_box_index: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum LineFragmentLookup {
    #[default]
    NotFound,
    LineOnly,
    Found,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct InlineAncestorChainRelativeOffset {
    pub(crate) offset_x: crate::layout::CssPixels,
    pub(crate) offset_y: crate::layout::CssPixels,
    pub(crate) found_fragmented_inline_node: bool,
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
    pub svg_viewport_size: crate::layout::FfiCssPixelSize,
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
    pub set_svg_viewport_size: unsafe extern "C" fn(*mut c_void, *mut c_void, crate::layout::FfiCssPixelSize),
    pub set_computed_svg_path: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub set_grid_layout_data: unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiGridLayoutData),
    pub set_flex_layout_data: unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiFlexLayoutData),
    pub set_used_grid_tracks:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *const FfiUsedGridTrackList, *const FfiUsedGridTrackList),
    pub finish_node:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut c_void, *mut c_void) -> FfiCommitNodeResult,
    pub assign_inline_box_geometry: unsafe extern "C" fn(*mut c_void, *mut c_void),
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PendingAbsposChild {
    pub(crate) child_box: Node,
    pub(crate) static_position_rect: StaticPositionRect,
    pub(crate) containing_block_info_override: Option<AbsposContainingBlockInfo>,
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

    fn computed_values_view_if_styled(&self) -> Option<ComputedValuesView<'pass>> {
        self.callbacks.computed_values_view_if_styled(self.node)
    }

    fn parent_computed_values_view_if_styled(&self) -> Option<ComputedValuesView<'pass>> {
        let parent = self.data().parent;
        if parent.is_invalid() {
            return None;
        }
        self.callbacks.computed_values_view_if_styled(parent)
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
        self.computed_values_view_if_styled().is_some_and(|style| style.is_floating())
    }

    pub(crate) fn is_absolutely_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.is_absolutely_positioned())
    }

    pub(crate) fn is_relatively_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.position() == crate::css::css_enums::positioning::RELATIVE)
    }

    pub(crate) fn is_in_flow(&self) -> bool {
        !crate::layout::node_is_out_of_flow(self.data(), self.computed_values_view_if_styled())
    }

    pub(crate) fn is_inline(&self) -> bool {
        crate::layout::kind_is_text(self.data().kind)
            || self
                .computed_values_view_if_styled()
                .is_some_and(|style| style.display().is_inline_outside())
    }

    pub(crate) fn is_atomic_inline(&self) -> bool {
        let data = self.data();
        crate::layout::has_flag(data, NodeFlag::IsReplacedElement)
            || data.kind == NodeKind::ListItemMarkerBox
            || self.computed_values_view_if_styled().is_some_and(|style| {
                let display = style.display();
                display.is_inline_outside() && !display.is_flow_inside()
            })
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
                && self.computed_values_view_if_styled().is_some_and(|style| {
                    let display = style.display();
                    display.is_inline_outside() && display.is_flow_inside()
                }))
    }

    pub(crate) fn is_inline_flow_interrupting_block(&self) -> bool {
        if !self.has_box_model_metrics() {
            return false;
        }
        let data = self.data();
        let Some(parent) = self.parent_data() else {
            return false;
        };
        let parent_is_inline_flow = self.parent_computed_values_view_if_styled().is_some_and(|style| {
            let display = style.display();
            display.is_inline_outside() && display.is_flow_inside()
        });
        if !parent_is_inline_flow {
            return false;
        }
        let style = self.computed_values_view_if_styled();
        if style.is_some_and(|style| style.display().is_inline_outside())
            || crate::layout::node_is_out_of_flow(data, style)
        {
            return false;
        }
        let display = self.display();
        if display.is_contents() {
            return false;
        }
        if display.is_internal_table() || display.is_table_caption() {
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
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.display_before_box_type_transformation().is_block_outside())
    }

    pub(crate) fn inline_axis_is_reverse(&self) -> bool {
        let style = self.style();
        match style.writing_mode() {
            writing_mode::HORIZONTAL_TB
            | writing_mode::VERTICAL_RL
            | writing_mode::VERTICAL_LR
            | writing_mode::SIDEWAYS_RL => style.direction() == 1,
            writing_mode::SIDEWAYS_LR => style.direction() == 0,
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
        crate::layout::has_flag(self.data(), NodeFlag::IsReplacedElement)
            && self
                .computed_values_view_if_styled()
                .is_some_and(|style| {
                    let display = style.display_before_box_type_transformation();
                    display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
                })
    }

    pub(crate) fn creates_block_formatting_context(&self) -> bool {
        crate::layout::node_creates_block_formatting_context(
            self.data(),
            self.computed_values_view_if_styled(),
            self.parent_computed_values_view_if_styled(),
        )
    }

    pub(crate) fn is_editing_host(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::IsEditingHost)
    }

    pub(crate) fn produces_line_box_fragment_when_empty(&self) -> bool {
        crate::layout::has_flag(self.data(), NodeFlag::ProducesLineBoxFragmentWhenEmpty)
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
            if child_data.kind == NodeKind::LegendBox
                && !crate::layout::node_is_out_of_flow(child_data, self.callbacks.computed_values_view_if_styled(child))
            {
                return child;
            }
            child = child_data.next_sibling;
        }
        Node::INVALID
    }

    pub(crate) fn list_item_marker(&self) -> Node {
        self.list_item().marker
    }

    pub(crate) fn marker_is_symbolic(&self) -> bool {
        self.list_item().marker_is_symbolic
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

    pub(crate) fn node_has_size_containment(&self) -> bool {
        let display = self.display();
        if display.is_table_inside() || display.is_internal_table() {
            return false;
        }
        let style = self.style();
        style.has_size_containment() || style.is_size_container()
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
        let (numerator, denominator) = style.css_preferred_aspect_ratio();
        (denominator != crate::layout::CssPixels::default()).then_some(PixelFraction { numerator, denominator })
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
        overflow_value_makes_box_a_scroll_container(style.overflow_x())
            || overflow_value_makes_box_a_scroll_container(style.overflow_y())
    }

    pub(crate) fn display(&self) -> crate::layout::FfiDisplay {
        if self.data().style.is_null() {
            return crate::layout::FfiDisplay::block();
        }
        self.state.style_facts(self.callbacks, self.node).display()
    }

    pub(crate) fn is_svg_box(&self) -> bool {
        crate::layout::kind_is_svg_box(self.data().kind)
    }

    pub(crate) fn is_svg_svg_box(&self) -> bool {
        self.data().kind == NodeKind::SVGSVGBox
    }

    pub(crate) fn is_table_box(&self) -> bool {
        self.display().is_table_inside()
    }

    pub(crate) fn is_table_wrapper(&self) -> bool {
        self.data().kind == NodeKind::TableWrapper
    }

    pub(crate) fn is_table_row_group(&self) -> bool {
        self.display().is_table_row_group()
    }

    pub(crate) fn is_table_header_group(&self) -> bool {
        self.display().is_table_header_group()
    }

    pub(crate) fn is_table_footer_group(&self) -> bool {
        self.display().is_table_footer_group()
    }

    pub(crate) fn is_table_row(&self) -> bool {
        self.display().is_table_row()
    }

    pub(crate) fn is_table_cell(&self) -> bool {
        self.display().is_table_cell()
    }

    pub(crate) fn is_table_column_group(&self) -> bool {
        self.display().is_table_column_group()
    }

    pub(crate) fn is_table_column(&self) -> bool {
        self.display().is_table_column()
    }

    pub(crate) fn is_table_caption(&self) -> bool {
        self.display().is_table_caption()
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
    contained_abspos_children: PagedStore<RefCell<VecDeque<PendingAbsposChild>>>,
    abspos_layout_pass_queue_in_completion_order: RefCell<VecDeque<Node>>,
    abspos_layout_pass_is_active: Cell<bool>,
    anchor_inset_store: AnchorInsetStore,
    replaced_content_facts: PagedStore<crate::layout::FfiReplacedContentFacts>,
    list_item_facts: PagedStore<crate::layout::FfiListItemFacts>,
    line_data: PagedStore<RefCell<LineData>>,
    block_rare_data: PagedStore<RefCell<BlockRareData>>,
    used_values_rare_data: PagedStore<RefCell<UsedValuesRareData>>,
    inline_containing_blocks: RefCell<HashSet<Node>>,
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
    pub(crate) svg_viewport_size: Option<crate::layout::FfiCssPixelSize>,
    pub(crate) grid_layout_data: Option<OwnedGridLayoutData>,
    pub(crate) flex_layout_data: Option<OwnedFlexLayoutData>,
    pub(crate) used_grid_tracks: Option<OwnedUsedGridTracks>,
    pub(crate) override_borders_data: Option<FfiBordersData>,
    pub(crate) abspos_layout_inputs: Option<AbsposLayoutInputs>,
    pub(crate) inline_containing_block_first_last_rect: Option<PhysicalRect>,
}

impl LayoutState {
    pub(crate) fn new(purpose: LayoutStatePurpose) -> Self {
        Self {
            used_values: PagedStore::default(),
            contained_abspos_children: PagedStore::default(),
            abspos_layout_pass_queue_in_completion_order: RefCell::new(VecDeque::new()),
            abspos_layout_pass_is_active: Cell::new(false),
            anchor_inset_store: AnchorInsetStore::default(),
            replaced_content_facts: PagedStore::default(),
            list_item_facts: PagedStore::default(),
            line_data: PagedStore::default(),
            block_rare_data: PagedStore::default(),
            used_values_rare_data: PagedStore::default(),
            inline_containing_blocks: RefCell::new(HashSet::new()),
            purpose,
        }
    }

    pub(crate) fn is_measurement(&self) -> bool {
        self.purpose == LayoutStatePurpose::Measurement
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

        let adjust_for_box_sizing = |unadjusted: crate::layout::CssPixels, computed_size: &ComputedSize, axis: Axis| {
            // box-sizing: content-box and automatic sizes need no
            // adjustment.
            if style.box_sizing() == box_sizing::CONTENT_BOX || computed_size.is_auto() {
                return unadjusted;
            }

            // box-sizing: border-box subtracts the relevant border and
            // padding. Block-axis padding percentages also resolve against
            // the containing block's inline size.
            let inline_basis = percentage_basis_inline_size.unwrap_or_default();
            let border_and_padding = match axis {
                Axis::Inline => {
                    style.border_left_width()
                        + style.padding_left().to_px(inline_basis)
                        + style.border_right_width()
                        + style.padding_right().to_px(inline_basis)
                }
                Axis::Block => {
                    style.border_top_width()
                        + style.padding_top().to_px(inline_basis)
                        + style.border_bottom_width()
                        + style.padding_bottom().to_px(inline_basis)
                }
            };
            unadjusted - border_and_padding
        };

        let parent = callbacks.parent(node);
        let parent_facts = (!parent.is_invalid()).then(|| self.node_facts(callbacks, parent));
        let is_definite_size = |size: &ComputedSize, axis: Axis| -> Option<crate::layout::CssPixels> {
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
            if size.contains_percentage() && !containing_block_has_definite_size(axis) {
                return None;
            }
            let basis = if size.contains_percentage() {
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

        self.used_values.allocate(slot_index, used)
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
        if self.node_facts(callbacks, node).is_svg_svg_box() {
            self.used_values_rare_data_mut(slot_index).svg_viewport_size = Some(geometry.svg_viewport_size);
        }

        Some(self.used_values.allocate(slot_index, used))
    }

    pub(crate) fn note_inline_containing_block(&self, inline_containing_block: Node) {
        self.inline_containing_blocks.borrow_mut().insert(inline_containing_block);
    }

    pub(crate) fn has_inline_containing_blocks(&self) -> bool {
        !self.inline_containing_blocks.borrow().is_empty()
    }

    pub(crate) fn is_inline_containing_block(&self, node: Node) -> bool {
        self.inline_containing_blocks.borrow().contains(&node)
    }

    pub(crate) fn inline_containing_block_first_last_rect(&self, slot_index: u32) -> Option<PhysicalRect> {
        self.used_values_rare_data(slot_index)?.inline_containing_block_first_last_rect
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
        StyleValues::new(
            callbacks.style_payloads(node),
            &self.anchor_inset_store,
            callbacks.slot_index(node),
        )
    }

    pub(crate) fn replace_resolved_anchor_insets(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        resolved: crate::layout::FfiResolvedAnchorInsets,
    ) {
        let slot_index = callbacks.slot_index(node);
        let replace = |field: InsetField, is_auto: bool, px: crate::layout::CssPixels| {
            self.anchor_inset_store
                .set_override(slot_index, field, ResolvedInsetOverride { is_auto, px });
        };
        if resolved.resolves_top {
            replace(InsetField::Top, resolved.top_is_auto, resolved.top);
        }
        if resolved.resolves_right {
            replace(InsetField::Right, resolved.right_is_auto, resolved.right);
        }
        if resolved.resolves_bottom {
            replace(InsetField::Bottom, resolved.bottom_is_auto, resolved.bottom);
        }
        if resolved.resolves_left {
            replace(InsetField::Left, resolved.left_is_auto, resolved.left);
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
            let style = self.style_facts(callbacks, node);
            style.has_size_containment() || style.is_size_container()
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
            white_space_collapse: parent_style.white_space_collapse(),
            word_break: parent_style.word_break(),
            font_variant_emoji: parent_style.font_variant_emoji(),
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
                .allocate(slot_index, RefCell::new(VecDeque::new()))
        });
        let mut children = children.borrow_mut();
        if cfg!(debug_assertions) {
            for entry in children.iter() {
                // Every box is laid out at most once per state, so it can only be registered once.
                assert_ne!(entry.child_box, child_box);
            }
        }
        // Entries are kept in tree order so consumption follows document order.
        let insertion_index = children.partition_point(|entry| callbacks.is_before(entry.child_box, child_box));
        children.insert(
            insertion_index,
            PendingAbsposChild {
                child_box,
                static_position_rect,
                containing_block_info_override: None,
            },
        );
    }

    pub(crate) fn has_contained_abspos_children(&self, target_box: Node) -> bool {
        self.contained_abspos_children
            .get(target_box.slot_index())
            .is_some_and(|children| !children.borrow().is_empty())
    }

    pub(crate) fn all_registered_contained_abspos_children_are_laid_out(&self) -> bool {
        let mut all_laid_out = true;
        self.contained_abspos_children.for_each(|children| {
            all_laid_out &= children.borrow().is_empty();
        });
        all_laid_out
    }

    /// Every completed root enqueues even when its queue is still empty:
    /// fixed-position descendants of abspos subtrees register against the
    /// viewport only while the pass lays those subtrees out.
    pub(crate) fn enqueue_for_abspos_layout_pass(&self, target_box: Node) {
        self.abspos_layout_pass_queue_in_completion_order
            .borrow_mut()
            .push_back(target_box);
    }

    pub(crate) fn pop_from_abspos_layout_pass_queue(&self) -> Option<Node> {
        self.abspos_layout_pass_queue_in_completion_order.borrow_mut().pop_front()
    }

    pub(crate) fn abspos_layout_pass_is_active(&self) -> bool {
        self.abspos_layout_pass_is_active.get()
    }

    pub(crate) fn set_abspos_layout_pass_is_active(&self, is_active: bool) {
        self.abspos_layout_pass_is_active.set(is_active);
    }

    /// By completion time the grid-area geometry is final and every abspos
    /// child registering against this containing block has done so.
    pub(crate) fn override_contained_abspos_child_containing_blocks(
        &self,
        target_box: Node,
        containing_block_info_for_child: impl Fn(Node) -> AbsposContainingBlockInfo,
    ) {
        let Some(children) = self.contained_abspos_children.get(target_box.slot_index()) else {
            return;
        };
        for entry in children.borrow_mut().iter_mut() {
            entry.containing_block_info_override = Some(containing_block_info_for_child(entry.child_box));
        }
    }

    pub(crate) fn take_next_contained_abspos_child(&self, target_box: Node) -> Option<PendingAbsposChild> {
        self.contained_abspos_children
            .get(target_box.slot_index())?
            .borrow_mut()
            .pop_front()
    }

    /// Accumulates relative-position insets from a chain of inline-flow
    /// ancestors, starting at first_ancestor and walking up until stop_at or
    /// the first ancestor that is not inline-flow.
    pub(crate) fn accumulated_relative_insets_from_inline_ancestor_chain(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        first_ancestor: Node,
        stop_at: Node,
    ) -> InlineAncestorChainRelativeOffset {
        let mut result = InlineAncestorChainRelativeOffset::default();
        let mut ancestor = first_ancestor;
        while !ancestor.is_invalid() && ancestor != stop_at {
            let facts = self.node_facts(callbacks, ancestor);
            if !facts.has_box_model_metrics() {
                break;
            }
            let display = facts.display();
            if !display.is_inline_outside() || !display.is_flow_inside() {
                break;
            }
            result.found_fragmented_inline_node |= facts.is_fragmented_inline();
            if facts.is_relatively_positioned() {
                // An inline that never went through inline layout this pass has
                // no used values; its committed box model is zeroed, so it
                // contributes no inset.
                if let Some(used) = self.try_used_values(callbacks, ancestor) {
                    result.offset_x += used.inset_left.get();
                    result.offset_y += used.inset_top.get();
                }
            }
            ancestor = callbacks.parent(ancestor);
        }
        result
    }

    /// Composes the offset the paintable receives: the containing line box
    /// fragment override for atomic inlines, the box's own relative-position
    /// inset, and the relative insets accumulated from a fragmented inline
    /// ancestor chain (block-in-inline). Offsets materialized from a previous
    /// layout's paintable already include all of these. Also returns the line
    /// box index to record for atomic inlines whose containing line survived
    /// line post-processing.
    fn committed_content_offset(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        used: &UsedValues,
    ) -> (crate::layout::FfiCssPixelPoint, Option<usize>) {
        let mut offset = used.content_offset.get();
        let mut containing_line_box_index = None;
        if !used.materialized_from_paintable.get() {
            let facts = self.node_facts(callbacks, node);
            if facts.is_box() && !facts.is_fragmented_inline() {
                let (lookup, line_fragment_offset) = self.line_fragment_lookup(callbacks, used);
                if lookup != LineFragmentLookup::NotFound {
                    containing_line_box_index = Some(used.containing_line_box_fragment.get().line_box_index);
                }
                if lookup == LineFragmentLookup::Found {
                    offset = line_fragment_offset;
                }
                if facts.is_relatively_positioned() {
                    offset.x += used.inset_left.get();
                    offset.y += used.inset_top.get();
                }
                if facts.is_in_flow() && facts.display().is_block_outside() {
                    let chain = self.accumulated_relative_insets_from_inline_ancestor_chain(
                        callbacks,
                        callbacks.parent(node),
                        callbacks.containing_block(node),
                    );
                    if chain.found_fragmented_inline_node {
                        offset.x += chain.offset_x;
                        offset.y += chain.offset_y;
                    }
                }
            }
        }
        (offset, containing_line_box_index)
    }

    fn line_fragment_lookup(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        used: &UsedValues,
    ) -> (LineFragmentLookup, crate::layout::FfiCssPixelPoint) {
        if !used.has_containing_line_box_fragment.get() {
            return (LineFragmentLookup::NotFound, crate::layout::FfiCssPixelPoint::default());
        }
        // SAFETY: Commit traverses the still-live C++ layout tree
        // synchronously with the pass callback table.
        let containing_block = callbacks.containing_block(used.node);
        assert!(!containing_block.is_invalid());
        let slot_index = callbacks.slot_index(containing_block);
        let Some(data) = self.line_data(slot_index) else {
            return (LineFragmentLookup::NotFound, crate::layout::FfiCssPixelPoint::default());
        };
        let coordinate = used.containing_line_box_fragment.get();
        let Some(line) = data.line_boxes.get(coordinate.line_box_index) else {
            return (LineFragmentLookup::NotFound, crate::layout::FfiCssPixelPoint::default());
        };
        let Some(fragment) = line.fragments.get(coordinate.fragment_index) else {
            return (LineFragmentLookup::LineOnly, crate::layout::FfiCssPixelPoint::default());
        };
        let (x, y) = fragment.offset();
        (LineFragmentLookup::Found, crate::layout::FfiCssPixelPoint { x, y })
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

        let mut has_pending_inline_box_geometry = false;
        if let Some(used) = used
            && !paintable.is_null()
        {
            let (content_offset, containing_line_box_index) = self.committed_content_offset(callbacks, node, used);
            // SAFETY: Every callback below copies its plain-data argument or
            // consumes one retained handle synchronously.
            unsafe {
                (sink.set_box_metrics)(
                    sink.context,
                    paintable,
                    FfiCommittedBoxMetrics {
                        content_offset,
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
                        containing_line_box_index: containing_line_box_index.unwrap_or(0),
                        has_containing_line_box_index: containing_line_box_index.is_some(),
                    },
                );
            }

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
                    has_pending_inline_box_geometry = self
                        .line_data(slot_index)
                        .is_some_and(|data| !data.inline_box_pieces.is_empty());
                }
            }

            let (transforms, viewport_size, path) = self
                .used_values_rare_data_mut_if_present(slot_index)
                .map_or((None, None, None), |mut rare| {
                    (
                        rare.computed_svg_transforms,
                        rare.svg_viewport_size,
                        rare.computed_svg_path.as_mut().map(RetainedLayoutHandle::take),
                    )
                });
            unsafe {
                if let Some(transforms) = transforms {
                    (sink.set_computed_svg_transforms)(sink.context, paintable, transforms);
                }
                if let Some(viewport_size) = viewport_size {
                    (sink.set_svg_viewport_size)(sink.context, paintable, viewport_size);
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

        if has_pending_inline_box_geometry {
            // Inline box geometry unites this block's piece rects with the box
            // models of its descendant inline paintables, which exist only now
            // that the whole subtree has committed.
            unsafe { (sink.assign_inline_box_geometry)(sink.context, paintable) };
        }
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
        self.commit_subtree(
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
