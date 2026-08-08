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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ContainingBlockGeometry {
    pub(crate) content_origin_in_entry_space: FfiCssPixelPoint,
    pub(crate) padding_left: CssPixels,
    pub(crate) padding_right: CssPixels,
    pub(crate) padding_top: CssPixels,
    pub(crate) padding_bottom: CssPixels,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
}

impl ContainingBlockGeometry {
    pub(crate) fn from_used_values(used: &UsedValues, content_origin_in_entry_space: FfiCssPixelPoint) -> Self {
        Self {
            content_origin_in_entry_space,
            padding_left: used.padding_left.get(),
            padding_right: used.padding_right.get(),
            padding_top: used.padding_top.get(),
            padding_bottom: used.padding_bottom.get(),
            content_inline_size: used.content_inline_size.get(),
            content_block_size: used.content_block_size.get(),
        }
    }

    pub(crate) fn padding_box_inline_size(&self) -> CssPixels {
        self.content_inline_size + self.padding_left + self.padding_right
    }

    pub(crate) fn padding_box_block_size(&self) -> CssPixels {
        self.content_block_size + self.padding_top + self.padding_bottom
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PendingAbsposChild {
    pub(crate) child_box: Node,
    pub(crate) coordinate_space_box: Node,
    pub(crate) static_position_rect: StaticPositionRect,
    pub(crate) containing_block_info_override: Option<AbsposContainingBlockInfo>,
    pub(crate) inline_containing_block: Node,
    pub(crate) inline_containing_block_rect: Option<PhysicalRect>,
}
pub(crate) struct LayoutState {
    anchor_inset_store: AnchorInsetStore,
    purpose: LayoutStatePurpose,
}

pub(crate) struct RunRecords {
    root: Node,
    map: RefCell<HashMap<u32, std::rc::Rc<UsedValues>>>,
}

impl RunRecords {
    pub(crate) fn new(root: Node, root_used: std::rc::Rc<UsedValues>) -> Self {
        let records = Self::new_unrooted(root);
        records.register(root, root_used);
        records
    }

    pub(crate) fn new_unrooted(root: Node) -> Self {
        Self {
            root,
            map: RefCell::new(HashMap::new()),
        }
    }

    pub(crate) fn register(&self, node: Node, used: std::rc::Rc<UsedValues>) {
        let previous = self.map.borrow_mut().insert(node.slot_index(), used);
        assert!(
            previous.is_none(),
            "slot {} registered twice in the run rooted at slot {}",
            node.slot_index(),
            self.root.slot_index()
        );
    }

    pub(crate) fn create_used_values(
        &self,
        state: &LayoutState,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> std::rc::Rc<UsedValues> {
        let used = state.create_used_values(callbacks, node, constraints);
        self.register(node, used.clone());
        used
    }

    #[track_caller]
    pub(crate) fn used_values(&self, node: Node) -> std::rc::Rc<UsedValues> {
        let caller = std::panic::Location::caller();
        self.used_values_if_owned(node).unwrap_or_else(|| {
            panic!(
                "the run rooted at slot {} does not own the record for slot {} (read at {caller})",
                self.root.slot_index(),
                node.slot_index(),
            )
        })
    }

    pub(crate) fn used_values_if_owned(&self, node: Node) -> Option<std::rc::Rc<UsedValues>> {
        self.map.borrow().get(&node.slot_index()).cloned()
    }
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

#[derive(Default)]
pub(crate) struct LineData {
    pub(crate) line_boxes: Vec<LineBoxData>,
    pub(crate) inline_box_pieces: Vec<InlineBoxPieceData>,
}

#[derive(Default)]
pub(crate) struct UsedValuesRareData {
    pub(crate) table_cell_coordinates: Option<FfiTableCellCoordinates>,
    pub(crate) computed_svg_path: Option<libgfx_rust::path::OwnedPath>,
    pub(crate) computed_svg_transforms: Option<crate::layout::FfiSvgComputedTransforms>,
    pub(crate) svg_viewport_size: Option<crate::layout::FfiCssPixelSize>,
    pub(crate) grid_layout_data: Option<OwnedGridLayoutData>,
    pub(crate) flex_layout_data: Option<OwnedFlexLayoutData>,
    pub(crate) used_grid_tracks: Option<OwnedUsedGridTracks>,
    pub(crate) override_borders_data: Option<FfiBordersData>,
    pub(crate) abspos_layout_inputs: Option<AbsposLayoutInputs>,
}

impl LayoutState {
    pub(crate) fn new(purpose: LayoutStatePurpose) -> Self {
        Self {
            anchor_inset_store: AnchorInsetStore::default(),
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
    ) -> std::rc::Rc<UsedValues> {
        assert!(!node.is_invalid());
        let facts = self.node_facts(callbacks, node);

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
        let used = UsedValues::default();

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

        std::rc::Rc::new(used)
    }

    pub(crate) fn populate_from_paintable(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        paintable: *mut c_void,
    ) -> Option<std::rc::Rc<UsedValues>> {
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
        let used = UsedValues::default();
        used.set_content_inline_size(geometry.content_inline_size);
        used.set_content_block_size(geometry.content_block_size);
        used.has_definite_inline_size.set(true);
        used.has_definite_block_size.set(true);
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
        // Materialization is this box's placement: the previous paintable's
        // committed geometry is final from the moment it is adopted.
        used.has_content_offset.set(true);
        used.seal_committed_box_metrics();

        if self.node_facts(callbacks, node).is_svg_svg_box() {
            used.rare_data_mut().svg_viewport_size = Some(geometry.svg_viewport_size);
        }
        Some(std::rc::Rc::new(used))
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

    /// Accumulates relative-position insets from a chain of inline-flow
    /// ancestors, starting at first_ancestor and walking up until stop_at or
    /// the first ancestor that is not inline-flow.
    pub(crate) fn accumulated_relative_insets_from_inline_ancestor_chain(
        &self,
        records: &RunRecords,
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
                // A relatively positioned inline-flow ancestor reachable from a
                // committed fragment or piece was entered by its inline
                // formatting context this pass, which created its used values
                // and resolved its insets.
                let used = records.used_values(ancestor);
                result.offset_x += used.inset_left.get();
                result.offset_y += used.inset_top.get();
            }
            ancestor = callbacks.parent(ancestor);
        }
        result
    }

    /// The line box index to record for atomic inlines whose containing line
    /// survived line post-processing.
    fn resolve_containing_line_box_index(
        &self,
        records: &RunRecords,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        containing_block: Node,
        coordinate: Option<LineBoxFragmentCoordinate>,
        placed_offset: FfiCssPixelPoint,
    ) -> Option<usize> {
        let coordinate = coordinate?;
        let facts = self.node_facts(callbacks, node);
        if !facts.is_non_fragmented_box() {
            return None;
        }
        assert!(!containing_block.is_invalid());
        let containing_block_used = records.used_values(containing_block);
        let data = containing_block_used.line_data_ref()?;
        let line = data.line_boxes.get(coordinate.line_box_index)?;
        if let Some(fragment) = line.fragments.get(coordinate.fragment_index) {
            let (x, y) = fragment.offset();
            debug_assert_eq!(
                crate::layout::FfiCssPixelPoint { x, y },
                placed_offset,
                "stored line fragment offset diverged from the placed offset (is_block_outside={})",
                facts.display().is_block_outside()
            );
        }
        Some(coordinate.line_box_index)
    }

    fn commit_subtree(
        &self,
        node: Node,
        parent_paintable: *mut c_void,
        insert_before_paintable: *mut c_void,
        callbacks: &FfiLayoutFcCallbacks,
        sink: &FfiCommitSink,
        scopes: &mut crate::layout::CommitScopes<'_>,
    ) {
        let slot_index = callbacks.slot_index(node);
        let entry = scopes.link_for_slot(slot_index);
        if let Some(link) = entry {
            callbacks.set_saved_abspos_layout_inputs(node, link.abspos_layout_inputs);
        }
        // SAFETY: The C++ sink owns paintables and copies every plain-data
        // input synchronously.
        let node_shell = callbacks.shell(node);
        let paintable = unsafe { (sink.prepare_node)(sink.context, node_shell, entry.is_some()) };

        let mut has_pending_inline_box_geometry = false;
        if let Some(link) = entry
            && !paintable.is_null()
        {
            let fragment = &link.fragment;
            // SAFETY: Every callback below copies its plain-data argument or
            // consumes one retained handle synchronously.
            unsafe {
                (sink.set_box_metrics)(
                    sink.context,
                    paintable,
                    FfiCommittedBoxMetrics {
                        content_offset: link.committed_offset,
                        content_inline_size: fragment.content_inline_size,
                        content_block_size: fragment.content_block_size,
                        margin_left: fragment.margin_left,
                        margin_right: fragment.margin_right,
                        margin_top: fragment.margin_top,
                        margin_bottom: fragment.margin_bottom,
                        border_left: fragment.border_left,
                        border_right: fragment.border_right,
                        border_top: fragment.border_top,
                        border_bottom: fragment.border_bottom,
                        padding_left: fragment.padding_left,
                        padding_right: fragment.padding_right,
                        padding_top: fragment.padding_top,
                        padding_bottom: fragment.padding_bottom,
                        inset_left: link.inset_left,
                        inset_right: link.inset_right,
                        inset_top: link.inset_top,
                        inset_bottom: link.inset_bottom,
                        containing_line_box_index: link.containing_line_box_index.unwrap_or(0),
                        has_containing_line_box_index: link.containing_line_box_index.is_some(),
                    },
                );
            }

            unsafe {
                if let Some(borders) = fragment.override_borders_data {
                    (sink.set_override_borders)(sink.context, paintable, borders);
                }
                if let Some(coordinates) = fragment.table_cell_coordinates {
                    (sink.set_table_cell_coordinates)(sink.context, paintable, coordinates);
                }
            }

            if let Some(line_data) = &fragment.line_data {
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
                    push_line_data(line_data, fragment.content_inline_size, callbacks, line_sink);
                    unsafe {
                        (sink.finish_line_data)(sink.context);
                    }
                    has_pending_inline_box_geometry = !line_data.inline_box_pieces.is_empty();
                }
            }

            unsafe {
                if let Some(transforms) = fragment.computed_svg_transforms {
                    (sink.set_computed_svg_transforms)(sink.context, paintable, transforms);
                }
                if let Some(viewport_size) = fragment.svg_viewport_size {
                    (sink.set_svg_viewport_size)(sink.context, paintable, viewport_size);
                }
                if let Some(path) = fragment.computed_svg_path.take() {
                    (sink.set_computed_svg_path)(sink.context, paintable, path.as_raw());
                }
            }
            if let Some(data) = &fragment.grid_layout_data {
                data.with_ffi_view(|view| {
                    unsafe { (sink.set_grid_layout_data)(sink.context, paintable, view) };
                });
            }
            if let Some(data) = &fragment.flex_layout_data {
                data.with_ffi_view(|view| {
                    unsafe { (sink.set_flex_layout_data)(sink.context, paintable, view) };
                });
            }
            if let Some(tracks) = &fragment.used_grid_tracks {
                tracks.with_ffi_views(|columns, rows| {
                    unsafe { (sink.set_used_grid_tracks)(sink.context, paintable, columns, rows) };
                });
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

        if let Some(link) = entry {
            scopes.open_scope(&link.fragment.children);
        }
        let mut child = callbacks.first_child(node);
        while !child.is_invalid() {
            let next = callbacks.next_sibling(child);
            self.commit_subtree(child, result.paintable_for_children, null_mut(), callbacks, sink, scopes);
            child = next;
        }
        if entry.is_some() {
            scopes.close_scope();
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
        pass_fragments: &crate::layout::CompletedPassFragments,
    ) {
        let mut scopes = crate::layout::CommitScopes::for_pass(pass_fragments);
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
            &mut scopes,
        );
        unsafe {
            (sink.finish_commit)(sink.context);
        }
    }
}
