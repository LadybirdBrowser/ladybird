/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use smallvec::{SmallVec, smallvec};

const MAXIMUM_BALANCING_ROUNDS: usize = 16;
const MAXIMUM_EARLY_BREAK_ATTEMPTS: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FragmentationStep {
    LayOut,
    Stop,
}

struct ColumnLayout {
    outcome: fragmentation::FragmentainerOutcome,
    consumed_block_size: CssPixels,
    baselines: formatting_context::DerivedBaselines,
    root_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    root: Option<fragment_tree::UnplacedRootFragment>,
}

#[derive(Clone, Copy)]
enum ColumnExtent {
    BlockSize(CssPixels),
    UntilForcedBreak,
    AllRemainingContent,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ColumnLayoutUse {
    BalancingProbe,
    ContainerContent,
}

/// What the layout of a block-level box hands back to the loop over its parent's children, as the box stands once it is
/// placed.
struct BlockLevelBoxOutcome {
    /// The offset of the content box in the containing block, or the origin while the box is not placed.
    content_offset: FfiCssPixelPoint,
    is_placed: bool,
    /// The sizes and box sides of the box, in the frame of its containing block.
    cells: used_values::RootInputCells,
    /// The baselines of the contents of the box, relative to its content box.
    content_baselines: DerivedBaselines,
    collapses_through: bool,
    is_invisible_for_line_clamp: bool,
    can_break_inside: bool,
    broken_inside: Option<BrokenInside>,
    /// The baselines the box gives its containing block, relative to the containing block's content box.
    baselines_in_containing_block: DerivedBaselines,
    /// The block end of the margin box in the containing block, where the layout of the box found it.
    margin_box_block_end: Option<CssPixels>,
    /// A rendered legend, which its fieldset places once it knows the fieldset's block-start border. Boxed, as the
    /// outcome of every block-level box carries it through deeply nested layout.
    deferred_legend: Option<Box<DeferredLegend>>,
}

/// A rendered legend laid out in its fieldset's flow, which the fieldset places later: where the flow would put the
/// legend's content box, and what placing it takes.
struct DeferredLegend {
    flow_position: geometry::LogicalOffset,
    placed_child: formatting_context::PlacedChild,
    fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
}

impl BlockLevelBoxOutcome {
    fn content_block_size(&self) -> CssPixels {
        self.cells.content_block_size
    }

    fn border_box_bottom(&self) -> CssPixels {
        self.cells.border_box_bottom(self.cells.uses_collapsing_borders_model)
    }

    fn border_box_block_size(&self) -> CssPixels {
        self.cells
            .border_box_block_size(self.cells.uses_collapsing_borders_model)
    }

    fn margin_bottom(&self) -> CssPixels {
        self.cells.margin_bottom
    }

    fn border_box_block_end(&self) -> CssPixels {
        self.content_offset.y + self.content_block_size() + self.border_box_bottom()
    }
}

/// A block-level box that continues in the next fragmentainer.
struct BrokenInside {
    token: std::rc::Rc<fragmentation::BlockBreakToken>,
    /// The block end of the content of the box, relative to its content box.
    block_end_of_content: CssPixels,
}

/// What a block-level box that interrupts the lines of inline content takes from them: the line box it shows on, if
/// any, and the offset the relative positioning of the fragmented inline boxes it interrupts gives its fragment.
#[derive(Clone, Copy)]
pub(crate) struct InterruptedInlineContent {
    line: Option<used_values::LineBoxFragmentCoordinate>,
    offset_from_relatively_positioned_inline_ancestors: FfiCssPixelPoint,
}

/// A block container whose children a block formatting context lays out, with the cells the container is laid out with.
#[derive(Clone, Copy)]
pub(crate) struct BlockContainer {
    pub(crate) node: Node,
    pub(crate) cells: used_values::RootInputCells,
    /// Whether the block-axis percentage insets of the boxes laid out in the container resolve against a definite
    /// block size.
    pub(crate) percentage_block_insets_have_definite_basis: bool,
}

impl BlockContainer {
    /// The run's root, laid out with `root_cells`, as the block container of the boxes laid out in it.
    pub(crate) fn root_of(run: &FormattingContextRun<'_>, root_cells: used_values::RootInputCells) -> Self {
        Self {
            node: run.box_,
            cells: root_cells,
            percentage_block_insets_have_definite_basis: run
                .percentage_block_insets_of_root_children_have_definite_basis(&root_cells),
        }
    }

    /// `node`, laid out with `cells` in `containing_block`, as the block container of the boxes laid out in it.
    fn laid_out_in(
        callbacks: &LayoutPass<'_>,
        node: Node,
        cells: used_values::RootInputCells,
        containing_block: BlockContainer,
    ) -> Self {
        Self {
            node,
            cells,
            percentage_block_insets_have_definite_basis:
                formatting_context::percentage_block_insets_have_definite_basis_in(
                    callbacks,
                    node,
                    || cells.has_definite_block_size,
                    containing_block.percentage_block_insets_have_definite_basis,
                ),
        }
    }
}

/// What the layout of a block-level box the parent flow holds leaves for its outcome.
struct LaidOutBlockLevelBox {
    broken_inside: Option<BrokenInside>,
    has_unbreakable_lines: bool,
    collapses_through: bool,
    /// The box as its layout left it: as a run of its own handed it back, or as this flow laid it out in cells its
    /// layout owns.
    laid_out_box: run_state::LaidOutChildBox,
    /// What this flow decided the box's fragment shows of it.
    fragment_facts: run_state::FragmentFactsDecidedByParent,
    /// The offset of the content box in the containing block, where this flow placed the box.
    content_offset: Option<FfiCssPixelPoint>,
    /// The block end of the margin box in the containing block, where this flow found it.
    margin_box_block_end: Option<CssPixels>,
    deferred_legend: Option<Box<DeferredLegend>>,
}

impl LaidOutBlockLevelBox {
    /// A box laid out as `laid_out_box` with `fragment_facts`, which neither broke inside nor let margins collapse
    /// through it, and which this flow did not place.
    fn of_box(
        laid_out_box: run_state::LaidOutChildBox,
        fragment_facts: run_state::FragmentFactsDecidedByParent,
    ) -> Self {
        Self {
            broken_inside: None,
            has_unbreakable_lines: false,
            collapses_through: false,
            laid_out_box,
            fragment_facts,
            content_offset: None,
            margin_box_block_end: None,
            deferred_legend: None,
        }
    }
}

/// The floats and margins of a block formatting context before finding where lines break changes them.
pub(crate) struct StateBeforeFindingLineBreaks {
    float_count: usize,
    bands: SmallVec<[FloatBand; 4]>,
    lowest_left_margin_edge: CssPixels,
    lowest_right_margin_edge: CssPixels,
    margin_state: BlockMarginState,
}

/// A float whose contents are laid out, which is yet to be positioned and placed.
pub(crate) struct FloatLaidOutAheadOfItsPosition {
    node: Node,
    placed_float: formatting_context::PlacedChild,
    pub(crate) cells: used_values::RootInputCells,
    laid_out_float_handed_back_by_its_run: Option<run_state::LaidOutChildBox>,
    fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
}

/// A box a block formatting context sizes, in cells the call laying the box out owns: the cells the context lays the
/// box out in, or the cells it sizes a child in ahead of the child's layout.
#[derive(Clone, Copy)]
pub(crate) struct ChildSizedHere<'sized> {
    pub(crate) node: Node,
    sized_in: SizedIn<'sized>,
}

#[derive(Clone, Copy)]
enum SizedIn<'sized> {
    LaidOutHere(&'sized Cell<used_values::RootInputCells>),
    CellsAheadOfItsRun(&'sized Cell<used_values::RootInputCells>),
}

impl<'sized> ChildSizedHere<'sized> {
    /// `node`, laid out in `cells`, which its layout owns.
    pub(crate) fn laid_out_in(node: Node, cells: &'sized Cell<used_values::RootInputCells>) -> Self {
        Self {
            node,
            sized_in: SizedIn::LaidOutHere(cells),
        }
    }

    /// `node`, a child sized in `cells` ahead of its layout, which takes them.
    pub(crate) fn ahead_of_its_run(node: Node, cells: &'sized Cell<used_values::RootInputCells>) -> Self {
        Self {
            node,
            sized_in: SizedIn::CellsAheadOfItsRun(cells),
        }
    }

    /// The cells the box is laid out in, if the context lays it out itself.
    fn cells_laid_out_here(self) -> Option<&'sized Cell<used_values::RootInputCells>> {
        match self.sized_in {
            SizedIn::LaidOutHere(cells) => Some(cells),
            SizedIn::CellsAheadOfItsRun(_) => None,
        }
    }
}

/// What laying out the children of a block container hands back to the layout of the container.
#[derive(Default)]
pub(crate) struct ChildrenSummary {
    pub(crate) baselines: DerivedBaselines,
    /// The lines laid out in the container, where an inline formatting context laid them out.
    pub(crate) lines: Option<used_values::LineData>,
    /// The break token of the container, where the fragmentainer stopped while its children were laid out.
    pub(crate) fragmentainer_stop_token: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
    /// Whether the lines of the container are kept together in one fragmentainer.
    pub(crate) lines_are_unbreakable: bool,
    /// The last line laid out in the container.
    pub(crate) last_line: Option<inline_content::LineRecord>,
    /// The last in-flow child whose margins do not collapse through it.
    pub(crate) last_in_flow_child: Option<LastInFlowChild>,
    /// The lowest block end of the border box of a child placed in the container, in the container's content box.
    pub(crate) block_end_of_placed_children: Option<CssPixels>,
    /// Whether the children laid out include the outside marker of the container, which the container places after
    /// them.
    pub(crate) includes_list_item_marker: bool,
    /// Whether margins do not collapse through one of the children laid out, which keeps them from collapsing through
    /// the container.
    pub(crate) has_child_whose_margins_do_not_collapse_through: bool,
}

#[derive(Clone, Copy)]
pub(crate) struct LastInFlowChild {
    /// The block end of the child's border box, in the container's content box.
    pub(crate) border_box_block_end: CssPixels,
    pub(crate) margin_bottom: CssPixels,
}

/// A child of the run's root that its flow laid out, as the automatic block size of the root reads it.
#[derive(Clone, Copy)]
pub(crate) struct RootFlowChild {
    pub(crate) node: Node,
    /// The block end of the child's border box, in the root's content box.
    pub(crate) border_box_block_end: CssPixels,
    pub(crate) border_box_block_size: CssPixels,
    pub(crate) margin_bottom: CssPixels,
    pub(crate) collapses_through: bool,
    pub(crate) is_invisible_for_line_clamp: bool,
}

impl RootFlowChild {
    fn of(node: Node, outcome: &BlockLevelBoxOutcome) -> Self {
        Self {
            node,
            border_box_block_end: outcome.border_box_block_end(),
            border_box_block_size: outcome.border_box_block_size(),
            margin_bottom: outcome.margin_bottom(),
            collapses_through: outcome.collapses_through,
            is_invisible_for_line_clamp: outcome.is_invisible_for_line_clamp,
        }
    }
}

impl ChildrenSummary {
    fn has_in_flow_content_edge(&self) -> bool {
        self.last_line.is_some() || self.last_in_flow_child.is_some()
    }

    fn has_unbreakable_lines(&self) -> bool {
        self.lines_are_unbreakable && self.last_line.is_some()
    }

    /// The block end of the content of the container: of its last line, or of the lowest child placed in it.
    fn block_end_of_content(&self) -> CssPixels {
        match self.last_line {
            Some(last_line) => last_line.physical_vertical_end(),
            None => self.block_end_of_placed_children.unwrap_or_default(),
        }
    }

    fn include_placed_child(&mut self, border_box_block_end: CssPixels) {
        self.block_end_of_placed_children = Some(
            self.block_end_of_placed_children
                .map_or(border_box_block_end, |lowest| lowest.max(border_box_block_end)),
        );
    }
}

struct FragmentationState {
    container_content_box_block_start_in_root: CssPixels,
    /// The break token of the container, once the fragmentainer stops inside it.
    container_stop_token: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
    resume: Option<fragmentation::BlockResume>,
    fragmentainer_had_content_before_container: bool,
    fragmentainer_had_content_before_child: bool,
    truncate_next_block_start_margin: bool,
    previous_in_flow_child_forces_break_after: bool,
    previous_in_flow_child_avoids_break_after: Option<bool>,
    last_child_allowing_break_before: Option<Node>,
}

impl FragmentationState {
    fn new(
        incoming: Option<&fragmentation::BlockBreakToken>,
        container_content_box_block_start_in_root: CssPixels,
        fragmentainer_had_content_before_container: bool,
        block_start_margin_is_truncated: bool,
    ) -> Self {
        let truncate_next_block_start_margin = block_start_margin_is_truncated
            || matches!(
                incoming.map(|token| &token.resume),
                Some(fragmentation::BlockResume::BeforeChild {
                    after_forced_break: false,
                    ..
                })
            );
        Self {
            container_content_box_block_start_in_root,
            container_stop_token: None,
            resume: incoming.map(|token| token.resume.clone()),
            fragmentainer_had_content_before_container,
            fragmentainer_had_content_before_child: false,
            truncate_next_block_start_margin,
            previous_in_flow_child_forces_break_after: false,
            previous_in_flow_child_avoids_break_after: None,
            last_child_allowing_break_before: None,
        }
    }
}

struct FloatAvoidanceProbe {
    opportunity: Option<CssPixels>,
    content_inline_size: Option<CssPixels>,
}

fn round_css_pixels(value: CssPixels) -> CssPixels {
    let half = CssPixels::from_raw(crate::layout::FIXED_POINT_DENOMINATOR >> 1);
    if value > CssPixels::default() {
        (value + half).floor()
    } else {
        (value - half).ceil()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PendingTopMarginGroup {
    pub(crate) box_: Node,
    pub(crate) collapsed_margin_at_open: CssPixels,
    pub(crate) collapsed_margin: CssPixels,
    pub(crate) open: bool,
    pub(crate) pinned_by_clearance: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct BlockMarginState {
    current_positive_collapsible_margin: CssPixels,
    current_negative_collapsible_margin: CssPixels,
    pending_top_margin_groups: Vec<PendingTopMarginGroup>,
    box_last_in_flow_child_margin_bottom_collapsed: bool,
}

impl BlockMarginState {
    pub(crate) fn add_margin(&mut self, margin: CssPixels) {
        if margin < CssPixels::default() {
            self.current_negative_collapsible_margin = margin.min(self.current_negative_collapsible_margin);
        } else {
            self.current_positive_collapsible_margin = margin.max(self.current_positive_collapsible_margin);
        }
    }

    pub(crate) fn current_collapsed_margin(&self) -> CssPixels {
        self.current_positive_collapsible_margin + self.current_negative_collapsible_margin
    }

    // Several ancestor groups may await placement, but only the last can remain open
    // and continue accumulating collapsed margins.
    pub(crate) fn open_top_margin_group(&mut self, box_: Node, pinned_by_clearance: bool) {
        let collapsed = self.current_collapsed_margin();
        self.pending_top_margin_groups.push(PendingTopMarginGroup {
            box_,
            collapsed_margin_at_open: collapsed,
            collapsed_margin: collapsed,
            open: true,
            pinned_by_clearance,
        });
    }

    pub(crate) fn take_pending_top_margin(&mut self) -> CssPixels {
        self.pending_top_margin_groups
            .pop()
            .expect("an open top-margin group exists")
            .collapsed_margin
    }

    pub(crate) fn has_open_top_margin_group(&self) -> bool {
        self.pending_top_margin_groups.last().is_some_and(|group| group.open)
    }

    pub(crate) fn pending_margin_for_next_box(&self) -> CssPixels {
        if self.has_open_top_margin_group() {
            return CssPixels::default();
        }
        self.current_collapsed_margin()
    }

    pub(crate) fn update_open_top_margin_group(&mut self) {
        if self.has_open_top_margin_group() {
            let collapsed = self.current_collapsed_margin();
            self.pending_top_margin_groups.last_mut().unwrap().collapsed_margin = collapsed;
        }
    }

    pub(crate) fn pending_top_margin_groups(&self) -> &[PendingTopMarginGroup] {
        &self.pending_top_margin_groups
    }

    pub(crate) fn reset(&mut self) {
        if self.has_open_top_margin_group() {
            self.pending_top_margin_groups.last_mut().unwrap().open = false;
        }
        self.current_negative_collapsible_margin = CssPixels::default();
        self.current_positive_collapsible_margin = CssPixels::default();
    }
}

/// What the probe for a stored in-flow layout found, and what storing the fresh layout needs.
#[expect(
    clippy::large_enum_variant,
    reason = "held on the stack while one child is laid out, where boxing would allocate for every child"
)]
enum InFlowReuse {
    Ineligible,
    Hit {
        entry: std::sync::Arc<in_flow_block_cache::InFlowBlockEntry>,
        fragment: std::sync::Arc<fragment_tree::Fragment>,
    },
    Store {
        key: in_flow_block_cache::InFlowBlockKey,
        validity: fc_run_cache::FcRunCacheValidity,
        flow_position: CssPixels,
        abspos_containing_block_info_contributions_received: u64,
    },
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FloatSide {
    Left,
    Right,
}

#[derive(Clone, Copy)]
pub(crate) struct FloatingBox {
    box_: Node,
    side: FloatSide,

    // Offset from left/right edge to the left content edge of `box`.
    offset_from_edge: CssPixels,

    // Distance from the left margin edge of `box` to its left content edge.
    left_margin_edge_to_content_edge: CssPixels,

    // Distance from the left content edge of `box` to its right margin edge.
    left_content_edge_to_right_margin_edge: CssPixels,

    // Bottom margin edge of `box`.
    bottom_margin_edge: CssPixels,

    margin_box_rect_in_root_coordinate_space: CssPixelRect,
    containing_block_rect_in_root_coordinate_space: CssPixelRect,
    percentage_basis_inline_size: Option<CssPixels>,
}

impl FloatingBox {
    fn margin_box_left_in_root_coordinate_space(&self) -> CssPixels {
        let containing_block_rect = self.containing_block_rect_in_root_coordinate_space;
        if self.side == FloatSide::Left {
            containing_block_rect.x + self.offset_from_edge - self.left_margin_edge_to_content_edge
        } else {
            containing_block_rect.right() - self.offset_from_edge - self.left_margin_edge_to_content_edge
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FloatBand {
    pub(crate) block_start: CssPixels,
    // NB: None means that no float on this side touches the band. A float can intrude by zero or a negative amount
    //     when it lies entirely outside the root, and it must still push later floats on the same side.
    pub(crate) left_intrusion: Option<CssPixels>,
    pub(crate) right_intrusion: Option<CssPixels>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct FloatPlacement {
    block_start: CssPixels,
    offset_from_edge: CssPixels,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum BreakMode {
    AllBreaks,
    ForcedBreaksOnly,
    NoBreaks,
}

// https://www.w3.org/TR/css-display/#block-formatting-context
pub(crate) struct BlockFormattingContext<'pass> {
    run: FormattingContextRun<'pass>,
    /// The cells of the run's root, in the frame its contents are laid out in. The root does not change while its
    /// contents are laid out.
    root_cells: used_values::RootInputCells,
    /// The lines the stored run of the root laid out, which inline layout of the root's own lines may reuse a prefix of.
    pub(super) previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    block_offset_of_current_block_container: Cell<Option<CssPixels>>,
    margin_state: RefCell<BlockMarginState>,
    floats: RefCell<Vec<FloatingBox>>,
    bands: RefCell<SmallVec<[FloatBand; 4]>>,
    lowest_left_margin_edge: Cell<CssPixels>,
    lowest_right_margin_edge: Cell<CssPixels>,
    lowest_floating_descendant_bottom_margin_edge: Cell<Option<CssPixels>>,
    derived_baselines_of_root_box: Cell<DerivedBaselines>,
    /// The trailing collapsed margin of the run, and the index in `root_flow_children` of the child it hangs below.
    trailing_collapsed_margin: Cell<Option<(usize, CssPixels)>>,
    table_box_in_wrapper_border_box_inline_size: Cell<Option<CssPixels>>,
    table_box_in_wrapper_border_box_block_size: Cell<Option<CssPixels>>,
    /// The inline layout of the table box inside a table wrapper child, as sizing the wrapper last found it, which the
    /// child's run can reuse.
    table_inline_layout_of_child: Cell<Option<(Node, table_formatting_context::TableInlineLayout)>>,
    is_line_clamp_container: bool,
    clamped_content_is_scrollable_overflow: bool,
    max_lines: Cell<Option<usize>>,
    line_clamp_point_found: Cell<bool>,
    line_clamp_line_count: Cell<usize>,
    automatic_line_clamp_block_size: Cell<Option<CssPixels>>,
    automatic_line_clamp_max_lines: Cell<Option<usize>>,
    automatic_line_clamp_block_size_exceeded: Cell<bool>,
    laying_out_invisible_line_clamp_content: Cell<bool>,
    break_mode: Cell<BreakMode>,
    column_inline_items: OnceCell<std::rc::Rc<inline_level_iterator::InlineItemCache>>,
    /// The children of the run's root that its flow laid out, in tree order.
    root_flow_children: RefCell<Vec<RootFlowChild>>,
    /// The lines laid out in the run's root, which the run finishes once it has sized the root.
    root_lines: RefCell<used_values::LineDataState>,
    column_early_breaks: RefCell<ColumnEarlyBreaks>,
    multicol_automatic_content_block_size: Cell<Option<CssPixels>>,
    discard_block_start_margin_of_next_child: Cell<bool>,
    fragmentainer_has_content: Cell<bool>,
    propagated_break_values: RefCell<HashMap<(Node, bool), u8>>,
    pending_continuation_token: RefCell<Option<std::rc::Rc<fragmentation::BlockBreakToken>>>,
    incoming_container_token: RefCell<Option<std::rc::Rc<fragmentation::BlockBreakToken>>>,
    break_before_child_is_avoided: Cell<Option<Node>>,
}

#[derive(Default)]
struct ColumnEarlyBreaks {
    block_size: Option<CssPixels>,
    columns: Vec<Option<ColumnEarlyBreak>>,
}

#[derive(Clone)]
struct ColumnEarlyBreak {
    incoming: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
    early_break_before: Option<Node>,
    allows_early_break_requests: bool,
}

impl ColumnEarlyBreaks {
    fn seed(
        &self,
        block_size: Option<CssPixels>,
        column_index: usize,
        incoming: Option<&fragmentation::BlockBreakToken>,
    ) -> Option<(Option<Node>, bool)> {
        if self.block_size != block_size {
            return None;
        }
        let column = self.columns.get(column_index)?.as_ref()?;
        (column.incoming.as_deref() == incoming)
            .then_some((column.early_break_before, column.allows_early_break_requests))
    }

    fn record(
        &mut self,
        block_size: Option<CssPixels>,
        column_index: usize,
        incoming: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
        seed: (Option<Node>, bool),
    ) {
        if self.block_size != block_size {
            self.block_size = block_size;
            self.columns.clear();
        }
        if self.columns.len() <= column_index {
            self.columns.resize(column_index + 1, None);
        }
        self.columns[column_index] = Some(ColumnEarlyBreak {
            incoming,
            early_break_before: seed.0,
            allows_early_break_requests: seed.1,
        });
    }
}

pub(crate) fn root_lays_out_content_against_its_own_block_size(
    sizing: &sizing_context::SizingContext<'_, '_>,
    root: Node,
) -> bool {
    let facts = sizing.facts(root);
    let outside_marker = facts.list_item_marker();
    let marker_resolves_against_list_item_block_size = facts.is_list_item_box()
        && !outside_marker.is_invalid()
        && !sizing.facts(outside_marker).list_marker_is_inside()
        && sizing.own_style_depends_on_percentage_block_size(outside_marker);
    facts.is_fieldset_box()
        || facts.lays_out_in_columns()
        || is_line_clamp_container(sizing.callbacks(), root)
        || facts.uses_button_layout()
        || facts.is_text_input()
        || facts.is_anonymous_button_content_wrapper()
        || facts.is_anonymous_button_content_box()
        || facts.is_table_wrapper()
        || marker_resolves_against_list_item_block_size
}

impl<'pass> BlockFormattingContext<'pass> {
    pub(crate) fn new(
        run: &FormattingContextRun<'pass>,
        root_cells: used_values::RootInputCells,
        previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    ) -> Self {
        let style = StyleValues::for_node(&run.callbacks, run.box_);
        let computed_continue = style.continue_();
        let is_line_clamp_container = is_line_clamp_container(&run.callbacks, run.box_);
        // AD-HOC: Content after a -webkit-legacy clamp point stays scrollable overflow, matching other engines. Sites
        //         detect clamped text by comparing scrollHeight with clientHeight.
        let clamped_content_is_scrollable_overflow =
            is_line_clamp_container && computed_continue == continue_value::_WEBKIT_LEGACY;
        Self {
            run: run.clone(),
            root_cells,
            previous_line_data,
            block_offset_of_current_block_container: Cell::new(None),
            margin_state: RefCell::new(BlockMarginState::default()),
            floats: RefCell::new(Vec::new()),
            bands: RefCell::new(smallvec![FloatBand::default()]),
            lowest_left_margin_edge: Cell::new(CssPixels::default()),
            lowest_right_margin_edge: Cell::new(CssPixels::default()),
            lowest_floating_descendant_bottom_margin_edge: Cell::new(None),
            derived_baselines_of_root_box: Cell::new(DerivedBaselines::default()),
            trailing_collapsed_margin: Cell::new(None),
            table_box_in_wrapper_border_box_inline_size: Cell::new(None),
            table_box_in_wrapper_border_box_block_size: Cell::new(None),
            table_inline_layout_of_child: Cell::new(None),
            is_line_clamp_container,
            clamped_content_is_scrollable_overflow,
            // NB: Measurements at a definite inline size need the clamped block size, just like committed layout.
            max_lines: Cell::new(
                ((run.fragments.is_some() || root_cells.has_definite_inline_size)
                    && is_line_clamp_container
                    && style.max_lines() > 0)
                    .then_some(style.max_lines() as usize),
            ),
            line_clamp_point_found: Cell::new(false),
            line_clamp_line_count: Cell::new(0),
            automatic_line_clamp_block_size: Cell::new(None),
            automatic_line_clamp_max_lines: Cell::new(None),
            automatic_line_clamp_block_size_exceeded: Cell::new(false),
            laying_out_invisible_line_clamp_content: Cell::new(false),
            break_mode: Cell::new(BreakMode::AllBreaks),
            column_inline_items: OnceCell::new(),
            root_flow_children: RefCell::new(Vec::new()),
            root_lines: RefCell::new(used_values::LineDataState::Empty),
            column_early_breaks: RefCell::new(ColumnEarlyBreaks::default()),
            multicol_automatic_content_block_size: Cell::new(None),
            discard_block_start_margin_of_next_child: Cell::new(false),
            fragmentainer_has_content: Cell::new(false),
            propagated_break_values: RefCell::new(HashMap::default()),
            pending_continuation_token: RefCell::new(None),
            incoming_container_token: RefCell::new(None),
            break_before_child_is_avoided: Cell::new(None),
        }
    }

    pub(crate) fn register_line_for_line_clamp(
        &self,
        container: Node,
        line: &mut line_box::LineBoxData,
        has_immediate_continuation: bool,
        line_block_end_in_bfc_root: CssPixels,
    ) -> bool {
        // https://drafts.csswg.org/css-overflow-4/#max-lines
        // If the box is a line-clamp container, its line-based clamp point is set to the first possible clamp point
        // after its Nth descendant in-flow line box.
        if self.line_clamp_point_found.get() || !self.is_line_clamp_container {
            return false;
        }
        let line_count = self.line_clamp_line_count.get() + 1;
        self.line_clamp_line_count.set(line_count);

        if let Some(automatic_block_size) = self.automatic_line_clamp_block_size.get() {
            if !self.automatic_line_clamp_block_size_exceeded.get() {
                if line_block_end_in_bfc_root <= automatic_block_size {
                    self.automatic_line_clamp_max_lines.set(Some(line_count));
                } else {
                    self.automatic_line_clamp_block_size_exceeded.set(true);
                    if self.automatic_line_clamp_max_lines.get().is_none() {
                        self.automatic_line_clamp_max_lines.set(Some(0));
                    }
                }
            }
            return false;
        }

        let Some(max_lines) = self.max_lines.get() else {
            return false;
        };
        if line_count != max_lines {
            return false;
        }
        let clamp_anchor = line
            .visible_fragments()
            .next_back()
            .map_or(container, |fragment| fragment.layout_node);
        let future_content = self.line_clamp_has_future_content(clamp_anchor);
        if has_immediate_continuation || future_content.is_some() {
            self.line_clamp_point_found.set(true);
        }
        has_immediate_continuation || future_content == Some(true)
    }
    pub(crate) fn has_line_clamp(&self) -> bool {
        self.is_line_clamp_container
    }
    pub(crate) fn clamped_content_is_scrollable_overflow(&self) -> bool {
        self.clamped_content_is_scrollable_overflow
    }
    pub(crate) fn line_clamp_reached(&self) -> bool {
        let reached = self.has_line_clamp() && self.line_clamp_point_found.get();
        reached && !self.laying_out_invisible_line_clamp_content.get()
    }

    pub(crate) fn table_box_in_wrapper_border_box_block_size(&self) -> Option<CssPixels> {
        self.table_box_in_wrapper_border_box_block_size.get()
    }

    #[inline]
    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.run.callbacks, node)
    }

    #[inline]
    fn style(&self, node: Node) -> StyleValues<'pass> {
        StyleValues::for_node(&self.run.callbacks, node)
    }

    /// Keeps `layout`, the inline layout of the table box inside `child`, for the run of `child`.
    pub(crate) fn keep_table_inline_layout_of(&self, child: Node, layout: table_formatting_context::TableInlineLayout) {
        self.table_inline_layout_of_child.set(Some((child, layout)));
    }

    /// The inline layout of the table box inside `child` this formatting context keeps for the run of `child`, if any.
    pub(crate) fn take_table_inline_layout_of(
        &self,
        child: Node,
    ) -> Option<table_formatting_context::TableInlineLayout> {
        take_table_inline_layout_kept_for(&self.table_inline_layout_of_child, child)
    }

    /// The run's root, as the block container of the boxes laid out in it.
    fn root_block_container(&self) -> BlockContainer {
        BlockContainer::root_of(&self.run, self.root_cells)
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.run.callbacks.next_sibling(node)
    }

    fn containing_block(&self, node: Node) -> Node {
        self.run.callbacks.in_flow_containing_block(node)
    }

    /// The boxes among the children of `node`.
    fn children(&self, node: Node) -> impl Iterator<Item = Node> + '_ {
        self.run
            .callbacks
            .children(node)
            .filter(|&child| self.facts(child).is_box())
    }

    fn is_ancestor_of(&self, ancestor: Node, node: Node) -> bool {
        self.run.callbacks.is_ancestor(ancestor, node, self.run.box_)
    }

    fn is_inclusive_ancestor_of(&self, ancestor: Node, node: Node) -> bool {
        ancestor == node || self.is_ancestor_of(ancestor, node)
    }

    #[inline]
    pub(crate) fn sizing(&self) -> sizing_context::SizingContext<'pass, '_> {
        sizing_context::SizingContext::new(self.run.state, self.run.callbacks)
    }

    /// The sizes, size constraints and box sides of `child`, a box this block formatting context sizes.
    fn metrics_of(&self, child: ChildSizedHere<'_>) -> used_values::RootInputCells {
        match child.sized_in {
            SizedIn::LaidOutHere(cells) | SizedIn::CellsAheadOfItsRun(cells) => cells.get(),
        }
    }

    fn update_cells_of(&self, child: ChildSizedHere<'_>, update: impl FnOnce(&mut used_values::RootInputCells)) {
        match child.sized_in {
            SizedIn::LaidOutHere(cells) | SizedIn::CellsAheadOfItsRun(cells) => cells.update_cells(update),
        }
    }

    /// The cells `child` is laid out in, a box this block formatting context lays out itself.
    #[track_caller]
    fn cells_laid_out_here<'cells>(&self, child: ChildSizedHere<'cells>) -> &'cells Cell<used_values::RootInputCells> {
        child
            .cells_laid_out_here()
            .unwrap_or_else(|| panic!("{:?} is sized ahead of its run, not laid out here", child.node))
    }

    /// `child`, a box this block formatting context lays out itself, as a box its sizing steps size there.
    #[track_caller]
    fn sized_cells_of<'cells>(&self, child: ChildSizedHere<'cells>) -> sizing_context::SizedCells<'cells> {
        sizing_context::SizedCells::new(child.node, self.cells_laid_out_here(child), self.run.callbacks.frame)
    }

    fn register_contained_abspos_child(&self, node: Node, block_offset: CssPixels, block_container: BlockContainer) {
        let coordinate_space_box = block_container.node;
        // https://drafts.csswg.org/css-position-3/#staticpos-rect
        // The static-position rectangle is a zero-thickness rectangle spanning between the inline-axis sides of the
        // box’s static-position containing block and positioned at its block-start static position (see CSS2§10.6.4).
        let static_position = abspos_inputs::StaticPositionRect {
            rect: geometry::LogicalRect {
                offset: geometry::LogicalOffset {
                    inline_offset: CssPixels::default(),
                    block_offset,
                },
                size: geometry::LogicalSize {
                    inline_size: block_container.cells.content_inline_size,
                    block_size: CssPixels::default(),
                },
            },
            inline_alignment: if self.style(coordinate_space_box).direction() == direction::RTL {
                StaticPositionAlignment::End
            } else {
                StaticPositionAlignment::Start
            },
            block_alignment: StaticPositionAlignment::Start,
            alignment_derives_from_own_computed_values: false,
            is_known: true,
        };
        formatting_context::register_contained_abspos_child(
            &self.run.callbacks,
            self.run.fragments.as_deref(),
            coordinate_space_box,
            node,
            static_position,
            None,
        );
    }

    /// Folds what `block_container` takes from `child` into the summary of the children laid out before it.
    fn fold_in_child(
        &self,
        summary: &mut ChildrenSummary,
        block_container: Node,
        child: Node,
        outcome: Option<&BlockLevelBoxOutcome>,
    ) {
        let container_facts = self.facts(block_container);
        if container_facts.is_list_item_box() && container_facts.list_item_marker() == child {
            summary.includes_list_item_marker = true;
        }
        let Some(outcome) = outcome else {
            return;
        };
        if block_container == self.run.box_ {
            self.root_flow_children
                .borrow_mut()
                .push(RootFlowChild::of(child, outcome));
        }
        if formatting_context::child_gives_container_baselines(&self.run.callbacks, block_container, child) {
            summary
                .baselines
                .fold_in_later_child(outcome.baselines_in_containing_block);
        }
        if outcome.is_placed {
            summary.include_placed_child(outcome.border_box_block_end());
        }
        let child_facts = self.facts(child);
        if !child_facts.is_floating_or_absolutely_positioned() && !outcome.collapses_through {
            summary.has_child_whose_margins_do_not_collapse_through = true;
        }
        // https://drafts.csswg.org/css-overflow-4/#line-clamp-containers
        // If a block container contains a clamp point, within itself or in any of its descendants, its automatic block
        // size will not take into account any invisible boxes, nor any clipped float.
        if !child_facts.is_absolutely_positioned()
            && !child_facts.is_floating()
            && !child_facts.is_list_item_marker_box()
            && !outcome.is_invisible_for_line_clamp
            && !outcome.collapses_through
        {
            summary.last_in_flow_child = Some(LastInFlowChild {
                border_box_block_end: outcome.border_box_block_end(),
                margin_bottom: outcome.margin_bottom(),
            });
        }
    }

    fn record_derived_baselines_of_root_box(&self, baselines: DerivedBaselines) {
        self.derived_baselines_of_root_box.set(baselines);
    }

    pub(crate) fn derived_baselines_of_root_box(&self) -> DerivedBaselines {
        self.derived_baselines_of_root_box.get()
    }

    /// Takes the lines laid out in the run's root, which the run finishes. Call this after the last read of
    /// `automatic_content_block_size`, which reads the last of these lines.
    pub(crate) fn take_root_lines(&self) -> used_values::LineDataState {
        std::mem::take(&mut *self.root_lines.borrow_mut())
    }

    /// Resolves the insets of `node`, an in-flow child of `block_container`, keeps them for the box and returns them.
    fn compute_inset(
        &self,
        run: &FormattingContextRun<'pass>,
        node: Node,
        block_container: BlockContainer,
    ) -> abspos_engine::InFlowBoxInsets {
        let block_container_cells = block_container.cells;
        abspos_engine::compute_inset_native(
            run,
            node,
            &block_container_cells,
            geometry::LogicalSize {
                inline_size: block_container_cells.content_inline_size,
                block_size: block_container_cells.content_block_size,
            },
            block_container.percentage_block_insets_have_definite_basis,
        )
    }

    fn containing_block_rect(&self, block_container: BlockContainer, position: FfiCssPixelPoint) -> CssPixelRect {
        let cells = block_container.cells;
        CssPixelRect {
            x: position.x,
            y: position.y,
            width: cells.content_inline_size,
            height: cells.content_block_size,
        }
    }

    fn margin_box_rect(metrics: &impl BoxMetricCells) -> CssPixelRect {
        let left = (metrics.margin_left() + metrics.border_box_left(false)).max(CssPixels::default());
        let right = (metrics.margin_right() + metrics.border_box_right(false)).max(CssPixels::default());
        let top = metrics.margin_box_top(false).max(CssPixels::default());
        let bottom = metrics.margin_box_bottom(false).max(CssPixels::default());
        CssPixelRect {
            x: -left,
            y: -top,
            width: left + metrics.content_inline_size() + right,
            height: top + metrics.content_block_size() + bottom,
        }
    }

    fn resolve_vertical_box_model_metrics(
        &self,
        sized_child: ChildSizedHere<'_>,
        containing_block_inline_size: CssPixels,
    ) {
        let node = sized_child.node;
        let style = self.style(node);
        self.update_cells_of(sized_child, |cells| {
            cells.resolve_vertical_box_model(&style, containing_block_inline_size);
        });
    }

    fn box_should_avoid_floats_because_it_establishes_fc(&self, node: Node) -> bool {
        box_avoids_floats_because_it_establishes_formatting_context(&self.run.callbacks, node)
    }

    // The definite inline space left for this box after the float bands that
    // intrude at its position are subtracted, including the negative-margin
    // adjustment; None when the space is not definite or the box may overlap
    // floats. This is the parent-owned half of block-level inline sizing —
    // it needs the flow position and the live float bands.
    fn float_reduced_inline_opportunity(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        content_position_in_root: FfiCssPixelPoint,
    ) -> Option<CssPixels> {
        let node = sized_child.node;
        if !matches!(available_space.inline_size, AvailableSize::Definite(_))
            || !self.box_should_avoid_floats_because_it_establishes_fc(node)
        {
            return None;
        }
        let style = self.style(node);
        let available_inline_size = available_space.inline_size.to_px_or_zero();
        let box_in_root_rect = CssPixelRect {
            x: content_position_in_root.x,
            y: content_position_in_root.y,
            width: available_inline_size,
            height: self.metrics_of(sized_child).content_block_size(),
        };
        // NB: The box avoids every float beside it, so this takes the deepest intrusions over its block size.
        let band = self.merged_band(box_in_root_rect.y, box_in_root_rect.bottom());
        let intrusion = self.intrusions_for_band_into_rect(band, box_in_root_rect);
        if intrusion.left <= CssPixels::default() && intrusion.right <= CssPixels::default() {
            return Some(available_inline_size);
        }
        // INTEROP: Like Chrome, the margins of the box may overlap the floats, so only the part of a positive margin
        //          that reaches past a float narrows the space next to it. Negative margins do not create additional
        //          space next to a float. The space for resolving an automatic inline size is the space next to the
        //          floats plus the margins, so that the resulting border box fills the space next to the floats.
        let zero = CssPixels::default();
        let margin_left = style.margin_left().to_px(available_inline_size);
        let margin_right = style.margin_right().to_px(available_inline_size);
        let opportunity = (available_inline_size
            - intrusion.left.max(margin_left.max(zero))
            - intrusion.right.max(margin_right.max(zero)))
        .max(zero);
        Some((opportunity + margin_left + margin_right).max(zero))
    }

    fn compute_inline_size(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        content_position_in_root: FfiCssPixelPoint,
    ) {
        let float_avoidance_inline_size =
            self.float_reduced_inline_opportunity(sized_child, available_space, content_position_in_root);
        let content_inline_size = self.resolve_root_inline_metrics_and_content_size(
            sized_child,
            available_space,
            constraints,
            float_avoidance_inline_size,
        );
        if let Some(content_inline_size) = content_inline_size {
            self.update_cells_of(sized_child, |cells| cells.set_content_inline_size(content_inline_size));
        }
    }

    fn resolve_root_inline_metrics_and_content_size(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        float_avoidance_inline_size: Option<CssPixels>,
    ) -> Option<CssPixels> {
        let node = sized_child.node;
        let facts = self.facts(node);
        let style = self.style(node);
        let mut remaining_available_space = available_space;
        if let Some(remaining_inline_size) = float_avoidance_inline_size {
            remaining_available_space.inline_size = AvailableSize::definite(remaining_inline_size);
        }

        let sizing = self.sizing();
        let available_inline_size = available_space.inline_size.to_px_or_zero();
        let calculate_inner_inline_size = |size: &ComputedSize| {
            if size.is_stretch()
                && let Some(remaining_inline_size) = float_avoidance_inline_size
            {
                // https://drafts.csswg.org/css-sizing-3/#stretch-fit-sizing
                // For in-flow block-level boxes that form an independent formatting context, use the space available
                // to line boxes (i.e. excluding floats) in place of the containing block's inline size.
                let stretch_fit_size = sizing.stretch_fit_inner_size(
                    sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                    SizingAxis::Inline,
                    remaining_inline_size,
                    constraints,
                );
                return sizing.resolve_calc_size(
                    sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                    SizingAxis::Inline,
                    size,
                    stretch_fit_size,
                    constraints.percentage_basis_inline_size,
                );
            }
            sizing.calculate_inner_inline_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space.inline_size,
                size,
                constraints,
            )
        };
        let sized_as_replaced = sizing.box_is_sized_as_replaced_element(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            available_space,
            constraints,
        );
        let replaced_inline_size = sized_as_replaced.then(|| {
            // 10.3.4 Block-level, replaced elements in normal flow:
            // the used value of 'width' is determined as for inline replaced
            // elements; the non-replaced rules below then resolve the
            // margins. Replaced sizing consults the box's own used metrics,
            // so they are written first.
            self.update_cells_of(sized_child, |cells| {
                cells.resolve_horizontal_box_model(&style, available_inline_size);
            });
            // NB: Replaced element sizing does not know about the floats a stretch inline size avoids.
            if style.width().is_stretch() && float_avoidance_inline_size.is_some() {
                return calculate_inner_inline_size(style.width());
            }
            sizing.compute_inline_size_for_replaced_element(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space,
                constraints,
            )
        });
        if sized_as_replaced && facts.is_floating() {
            // 10.3.6 Floating, replaced elements:
            // https://www.w3.org/TR/CSS22/visudet.html#float-replaced-width
            return replaced_inline_size;
        }

        if facts.is_floating() {
            // 10.3.5 Floating, non-replaced elements:
            // https://www.w3.org/TR/CSS22/visudet.html#float-width
            return Some(self.compute_floating_root_inline_sizes(sized_child, available_space, constraints));
        }

        let mut margin_left_is_auto = style.margin_left().is_auto();
        let mut margin_right_is_auto = style.margin_right().is_auto();
        let mut margin_left = style.margin_left().to_px(available_inline_size);
        let mut margin_right = style.margin_right().to_px(available_inline_size);
        let padding_left = style.padding_left().to_px(available_inline_size);
        let padding_right = style.padding_right().to_px(available_inline_size);
        let border_left_width = style.border_left_width();
        let border_right_width = style.border_right_width();
        self.update_cells_of(sized_child, |cells| {
            cells.margin_left = margin_left;
            cells.margin_right = margin_right;
            cells.border_left = border_left_width;
            cells.border_right = border_right_width;
            cells.padding_left = padding_left;
            cells.padding_right = padding_right;
        });
        // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
        // A rendered legend with a computed inline size of auto uses the
        // fit-content size, resolved here so the legend's children are laid
        // out against the used size rather than a provisional stretch size.
        if style.width().is_auto() {
            let container = self.containing_block(node);
            if !container.is_invalid()
                && self.facts(container).is_fieldset_box()
                && self.facts(container).rendered_legend() == node
            {
                return Some(sizing.calculate_fit_content_size(
                    sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                    SizingAxis::Inline,
                    remaining_available_space,
                    constraints,
                ));
            }
        }

        // An auto margin that is not honoured resolves to zero and stops taking part in the remaining steps.
        fn zero_out_auto_margin(margin: &mut CssPixels, is_auto: &mut bool) {
            if *is_auto {
                *margin = CssPixels::default();
                *is_auto = false;
            }
        }

        let remaining_inline_size = remaining_available_space.inline_size.to_px_or_zero();
        let containing_block_direction = self.style(self.containing_block(node)).direction();
        let computed_margin_left = style.margin_left();
        let computed_margin_right = style.margin_right();
        // NB: A box that establishes an orthogonal flow and has an automatic inline size is sized to its content, not
        //     over-constrained: its margins keep their values, unless the line-right margin has to place it at the
        //     inline start of a right-to-left containing block.
        let is_content_sized_orthogonal_flow = sizing.establishes_orthogonal_flow(node)
            && sizing.should_treat_inline_size_as_auto(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space,
            );
        let compute = |input: Option<CssPixels>,
                       margin_left: &mut CssPixels,
                       margin_right: &mut CssPixels,
                       margin_left_is_auto: &mut bool,
                       margin_right_is_auto: &mut bool|
         -> Option<CssPixels> {
            *margin_left = computed_margin_left.to_px(available_inline_size);
            *margin_right = computed_margin_right.to_px(available_inline_size);
            *margin_left_is_auto = computed_margin_left.is_auto();
            *margin_right_is_auto = computed_margin_right.is_auto();
            let mut inline_size = input;
            let mut total = border_left_width
                + border_right_width
                + *margin_left
                + padding_left
                + inline_size.unwrap_or_default()
                + padding_right
                + *margin_right;

            if !facts.is_inline() {
                // 10.3.3 Block-level, non-replaced elements in normal flow
                // If 'width' is not 'auto' and 'border-left-width' + 'padding-left' + 'width' + 'padding-right' +
                // 'border-right-width' (plus any of 'margin-left' or 'margin-right' that are not 'auto') is larger than the
                // width of the containing block, then any 'auto' values for 'margin-left' or 'margin-right' are, for the
                // following rules, treated as zero.
                if inline_size.is_some() && total > remaining_inline_size {
                    zero_out_auto_margin(margin_left, margin_left_is_auto);
                    zero_out_auto_margin(margin_right, margin_right_is_auto);
                    total = border_left_width
                        + border_right_width
                        + *margin_left
                        + padding_left
                        + inline_size.unwrap_or_default()
                        + padding_right
                        + *margin_right;
                }

                // 10.3.3 cont'd.
                let underflow = remaining_inline_size - total;
                if inline_size.is_none() {
                    zero_out_auto_margin(margin_left, margin_left_is_auto);
                    zero_out_auto_margin(margin_right, margin_right_is_auto);
                    debug_assert!(matches!(available_space.inline_size, AvailableSize::Definite(_)));
                    inline_size = Some(underflow.max(CssPixels::default()));
                } else if !*margin_left_is_auto && !*margin_right_is_auto {
                    if containing_block_direction == direction::RTL {
                        *margin_left += underflow;
                    } else if !is_content_sized_orthogonal_flow {
                        *margin_right += underflow;
                    }
                } else if !*margin_left_is_auto && *margin_right_is_auto {
                    *margin_right = underflow;
                    *margin_right_is_auto = false;
                } else if *margin_left_is_auto && !*margin_right_is_auto {
                    *margin_left = underflow;
                    *margin_left_is_auto = false;
                } else {
                    let half = underflow / 2;
                    *margin_left = half;
                    *margin_right = half;
                    *margin_left_is_auto = false;
                    *margin_right_is_auto = false;
                }
            }
            inline_size
        };

        let input_inline_size = if sized_as_replaced {
            // NOTE: Replaced elements had their inline size calculated independently above.
            //       We use that inline size as the input here to ensure that margins get resolved.
            replaced_inline_size
        } else if facts.is_table_wrapper() {
            // NB: An auto-width table shrinks to fit the space next to floats. A table with a specified width does not,
            //     so clamp it against the containing block instead. That way a table which is too wide for the space
            //     next to floats reports its real size and gets placed below the floats.
            let table_has_specified_inline_size = !self.style(sizing.table_box_inside_wrapper(node)).width().is_auto();
            let (inline_size, table_inline_layout) = sizing.compute_table_box_inline_size_inside_wrapper(
                node,
                remaining_available_space,
                constraints,
                table_has_specified_inline_size.then_some(available_inline_size),
                formatting_context::TableWrapperInlineSizeMode::ClampToAvailableInlineSize,
            );
            if node != self.run.box_ {
                self.keep_table_inline_layout_of(node, table_inline_layout);
            }
            Some(inline_size)
        } else if facts.uses_button_layout() && style.width().is_auto() {
            // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
            // If the computed value of 'inline-size' is 'auto', then the used value is the fit-content inline size.
            Some(sizing.calculate_fit_content_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                SizingAxis::Inline,
                available_space,
                constraints,
            ))
        } else if is_content_sized_orthogonal_flow {
            // https://drafts.csswg.org/css-writing-modes-4/#orthogonal-shrink-to-fit
            // Note that this section requires that when a child box auto-sized in its block axis establishes an
            // orthogonal flow, the used block size of the child is calculated to fit its content
            Some(sizing.calculate_fit_content_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                SizingAxis::Inline,
                remaining_available_space,
                constraints,
            ))
        } else if sizing.should_treat_inline_size_as_auto(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            available_space,
        ) {
            None
        } else {
            Some(calculate_inner_inline_size(style.width()))
        };

        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut used_inline_size = compute(
            input_inline_size,
            &mut margin_left,
            &mut margin_right,
            &mut margin_left_is_auto,
            &mut margin_right_is_auto,
        );

        // NB: A calc-size() with an auto basis resolves against the inline size auto gives the box.
        if style.width().is_auto()
            && style.width().calc_size().is_some()
            && let Some(auto_inline_size) = used_inline_size
        {
            let inline_size = sizing.resolve_calc_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                SizingAxis::Inline,
                style.width(),
                auto_inline_size,
                constraints.percentage_basis_inline_size,
            );
            used_inline_size = compute(
                Some(inline_size),
                &mut margin_left,
                &mut margin_right,
                &mut margin_left_is_auto,
                &mut margin_right_is_auto,
            );
        }

        // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let max_inline_size = calculate_inner_inline_size(style.max_width());
            if used_inline_size.unwrap_or_default() > max_inline_size {
                used_inline_size = compute(
                    Some(max_inline_size),
                    &mut margin_left,
                    &mut margin_right,
                    &mut margin_left_is_auto,
                    &mut margin_right_is_auto,
                );
            }
        }

        if !sized_as_replaced
            && let Some(value) = used_inline_size
            && let Some(limits) = sizing.transferred_inline_size_limits(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space,
                constraints,
            )
        {
            let limited = limits.clamp(value);
            if limited != value {
                used_inline_size = compute(
                    Some(limited),
                    &mut margin_left,
                    &mut margin_right,
                    &mut margin_left_is_auto,
                    &mut margin_right_is_auto,
                );
            }
        }

        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        let min_width = style.min_width();
        if !crate::layout::sizing_context::is_plain_auto(min_width)
            && let Some(value) = used_inline_size
        {
            let min_inline_size = calculate_inner_inline_size(min_width);
            if value < min_inline_size {
                used_inline_size = compute(
                    Some(min_inline_size),
                    &mut margin_left,
                    &mut margin_right,
                    &mut margin_left_is_auto,
                    &mut margin_right_is_auto,
                );
            }
        }

        self.update_cells_of(sized_child, |cells| {
            cells.margin_left = margin_left;
            cells.margin_right = margin_right;
        });
        if sized_as_replaced {
            replaced_inline_size
        } else {
            used_inline_size
        }
    }

    fn compute_floating_root_inline_sizes(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized_child.node;
        // 10.3.5 Floating, non-replaced elements
        let style = self.style(node);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();

        // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
        let margin_left = style.margin_left().to_px(containing_block_inline_size);
        let margin_right = style.margin_right().to_px(containing_block_inline_size);
        let padding_left = style.padding_left().to_px(containing_block_inline_size);
        let padding_right = style.padding_right().to_px(containing_block_inline_size);
        self.update_cells_of(sized_child, |cells| {
            cells.padding_left = padding_left;
            cells.padding_right = padding_right;
            cells.margin_left = margin_left;
            cells.margin_right = margin_right;
            cells.border_left = style.border_left_width();
            cells.border_right = style.border_right_width();
        });
        let sizing = self.sizing();
        let compute = |input: Option<CssPixels>| -> CssPixels {
            if let Some(value) = input {
                return value;
            }
            // If 'width' is computed as 'auto', the used value is the "shrink-to-fit" width.
            if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                // Find the available inline size: in this case, this is the inline size of the containing
                // block minus the used values of 'margin-left', 'border-left-width', 'padding-left',
                // 'padding-right', 'border-right-width', 'margin-right', and the widths of any relevant scroll bars.
                let available_inline_size = available_space.inline_size.to_px_or_zero()
                    - margin_left
                    - style.border_left_width()
                    - padding_left
                    - padding_right
                    - style.border_right_width()
                    - margin_right;
                // Then the shrink-to-fit inline size is:
                // min(max(preferred minimum inline size, available inline size), preferred inline size).
                let preferred = sizing.calculate_max_content_inline_size(
                    sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                    constraints,
                );
                if preferred <= available_inline_size {
                    preferred
                } else {
                    sizing
                        .calculate_min_content_inline_size(
                            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                            constraints,
                        )
                        .max(available_inline_size)
                        .min(preferred)
                }
            } else {
                // Fold the shrink-to-fit formula for an indefinite available inline size.
                sizing.calculate_max_content_inline_size(
                    sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                    constraints,
                )
            }
        };

        let input = if sizing.should_treat_inline_size_as_auto(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            available_space,
        ) {
            None
        } else {
            Some(sizing.calculate_inner_inline_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space.inline_size,
                style.width(),
                constraints,
            ))
        };
        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut inline_size = compute(input);
        // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let maximum = sizing.calculate_inner_inline_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space.inline_size,
                style.max_width(),
                constraints,
            );
            if inline_size > maximum {
                inline_size = compute(Some(maximum));
            }
        }
        if let Some(limits) = sizing.transferred_inline_size_limits(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            available_space,
            constraints,
        ) {
            let limited = limits.clamp(inline_size);
            if limited != inline_size {
                inline_size = compute(Some(limited));
            }
        }
        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        if !crate::layout::sizing_context::is_plain_auto(style.min_width()) {
            let minimum = sizing.calculate_inner_inline_size(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space.inline_size,
                style.min_width(),
                constraints,
            );
            if inline_size < minimum {
                inline_size = compute(Some(minimum));
            }
        }
        inline_size
    }

    pub(crate) fn resolve_used_block_size_if_not_treated_as_auto(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        let node = sized_child.node;
        let resolved = self.sizing().used_block_size_if_not_treated_as_auto(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            available_space,
            constraints,
        );
        if let Some(resolved) = resolved {
            self.update_cells_of(sized_child, |cells| resolved.apply_to_cells(cells));
        }
    }

    /// Resolves the block size of the box `sized_child` sizes where it is treated as automatic. `children` sums up the
    /// children of the box this flow laid out, where it laid them out.
    pub(crate) fn resolve_used_block_size_if_treated_as_auto(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        child_automatic_block_size: Option<CssPixels>,
        children: Option<&ChildrenSummary>,
    ) {
        let node = sized_child.node;
        let resolved = self.sizing().used_block_size_if_treated_as_auto(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            available_space,
            constraints,
            child_automatic_block_size,
            || {
                self.compute_automatic_block_size_for_block_level_element(
                    sized_child,
                    self.metrics_of(sized_child)
                        .available_inner_space_or_constraints_from(available_space),
                    constraints,
                    None,
                    children,
                )
            },
        );
        if let Some(resolved) = resolved {
            self.update_cells_of(sized_child, |cells| resolved.apply_to_cells(cells));
        }
    }

    fn band_index_at(&self, block_offset: CssPixels) -> usize {
        let bands = self.bands.borrow();
        assert!(!bands.is_empty());
        let mut index = 0;
        for (candidate, band) in bands.iter().enumerate().skip(1) {
            if band.block_start > block_offset {
                break;
            }
            index = candidate;
        }
        index
    }

    fn band_at(&self, block_offset: CssPixels) -> FloatBand {
        self.bands.borrow()[self.band_index_at(block_offset)]
    }

    pub(crate) fn next_float_band_block_start_after(&self, block_offset_in_root: CssPixels) -> Option<CssPixels> {
        self.bands
            .borrow()
            .iter()
            .find(|band| band.block_start > block_offset_in_root)
            .map(|band| band.block_start)
    }

    /// The deepest intrusion on each side of the bands between the two block offsets.
    fn merged_band(&self, block_start_in_root: CssPixels, block_end_in_root: CssPixels) -> FloatBand {
        let bands = self.bands.borrow();
        assert!(!bands.is_empty());
        let first_index = self.band_index_at(block_start_in_root);
        if block_end_in_root <= block_start_in_root {
            return bands[first_index];
        }
        let mut merged = FloatBand {
            block_start: block_start_in_root,
            ..FloatBand::default()
        };
        for band in bands.iter().skip(first_index) {
            if band.block_start >= block_end_in_root {
                break;
            }
            merged.left_intrusion = merged.left_intrusion.max(band.left_intrusion);
            merged.right_intrusion = merged.right_intrusion.max(band.right_intrusion);
        }
        merged
    }

    fn intrusions_for_band_into_rect(
        &self,
        band: FloatBand,
        rect_in_root: CssPixelRect,
    ) -> inline_formatting_context::SpaceUsedByFloats {
        let root_content_inline_size = self.root_cells.content_inline_size;
        inline_formatting_context::SpaceUsedByFloats {
            left: band.left_intrusion.map_or(CssPixels::default(), |intrusion| {
                (intrusion - rect_in_root.x).max(CssPixels::default())
            }),
            right: band.right_intrusion.map_or(CssPixels::default(), |intrusion| {
                (intrusion - (root_content_inline_size - rect_in_root.right())).max(CssPixels::default())
            }),
        }
    }

    pub(crate) fn intrusion_by_floats_into_rect(
        &self,
        box_in_root_rect: FfiCssPixelRect,
        block_start_in_box: CssPixels,
        block_end_in_box: CssPixels,
    ) -> inline_formatting_context::SpaceUsedByFloats {
        let rect = CssPixelRect {
            x: box_in_root_rect.x,
            y: box_in_root_rect.y,
            width: box_in_root_rect.width,
            height: box_in_root_rect.height,
        };
        let band = self.merged_band(rect.y + block_start_in_box, rect.y + block_end_in_box);
        self.intrusions_for_band_into_rect(band, rect)
    }

    fn place_float(
        &self,
        side: FloatSide,
        float_box: &impl BoxMetricCells,
        available_space: AvailableSpace,
        containing_block_rect_in_root: CssPixelRect,
        ceiling_in_root: CssPixels,
    ) -> FloatPlacement {
        let margin_box_inline_size = float_box.margin_box_inline_size(false);
        let mut candidate_block_start = ceiling_in_root;
        loop {
            let band = self.band_at(candidate_block_start);
            let intrusions = self.intrusions_for_band_into_rect(band, containing_block_rect_in_root);
            let available_inline_size =
                available_space.inline_size.to_px_or_zero() - intrusions.left - intrusions.right;
            let has_floats_present = band.left_intrusion.is_some() || band.right_intrusion.is_some();
            let fits = available_space.inline_size == AvailableSize::Indefinite
                || margin_box_inline_size <= available_inline_size
                || !has_floats_present;
            if !fits && let Some(next) = self.next_float_band_block_start_after(candidate_block_start) {
                candidate_block_start = next;
                continue;
            }
            let offset_from_edge = if side == FloatSide::Left {
                intrusions.left + float_box.margin_left() + float_box.border_box_left(false)
            } else {
                intrusions.right
                    + float_box.content_inline_size()
                    + float_box.margin_right()
                    + float_box.border_box_right(false)
            };
            return FloatPlacement {
                block_start: candidate_block_start,
                offset_from_edge,
            };
        }
    }

    fn ensure_band_boundary(&self, block_start: CssPixels) {
        let mut bands = self.bands.borrow_mut();
        assert!(!bands.is_empty());
        for index in 0..bands.len() {
            if bands[index].block_start == block_start {
                return;
            }
            if bands[index].block_start > block_start {
                let mut band = if index == 0 {
                    FloatBand::default()
                } else {
                    bands[index - 1]
                };
                band.block_start = block_start;
                bands.insert(index, band);
                return;
            }
        }
        let mut band = *bands.last().unwrap();
        band.block_start = block_start;
        bands.push(band);
    }

    fn add_float_to_bands(&self, floating_box: FloatingBox, mut containing_block_rect_in_root: CssPixelRect) {
        let pending_adjustment =
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(floating_box.box_);
        containing_block_rect_in_root.y += pending_adjustment;
        let root_content_inline_size = self.root_cells.content_inline_size;
        let margin_box_rect = floating_box
            .margin_box_rect_in_root_coordinate_space
            .translated(CssPixels::default(), pending_adjustment);
        let block_start = margin_box_rect.y;
        let block_end = margin_box_rect.bottom();
        if floating_box.side == FloatSide::Left {
            self.lowest_left_margin_edge
                .set(self.lowest_left_margin_edge.get().max(block_end));
        } else {
            self.lowest_right_margin_edge
                .set(self.lowest_right_margin_edge.get().max(block_end));
        }
        if block_end <= block_start {
            return;
        }
        // NB: A float that takes up no inline space only moves later floats and clearance down, which the lowest
        //     margin edges above track. It leaves the bands alone, so that it does not push later floats past it.
        if margin_box_rect.width <= CssPixels::default() {
            return;
        }
        self.ensure_band_boundary(block_start);
        self.ensure_band_boundary(block_end);
        let intrusion = if floating_box.side == FloatSide::Left {
            containing_block_rect_in_root.x
                + floating_box.offset_from_edge
                + floating_box.left_content_edge_to_right_margin_edge
        } else {
            (root_content_inline_size - containing_block_rect_in_root.right())
                + floating_box.offset_from_edge
                + floating_box.left_margin_edge_to_content_edge
        };
        for band in self.bands.borrow_mut().iter_mut() {
            if band.block_start < block_start {
                continue;
            }
            if band.block_start >= block_end {
                break;
            }
            if floating_box.side == FloatSide::Left {
                band.left_intrusion = band.left_intrusion.max(Some(intrusion));
            } else {
                band.right_intrusion = band.right_intrusion.max(Some(intrusion));
            }
        }
    }

    fn rebuild_float_bands(&self) {
        {
            let mut bands = self.bands.borrow_mut();
            bands.clear();
            bands.push(FloatBand::default());
        }
        self.lowest_left_margin_edge.set(CssPixels::default());
        self.lowest_right_margin_edge.set(CssPixels::default());
        let floats = self.floats.borrow();
        for &floating_box in floats.iter() {
            self.add_float_to_bands(
                floating_box,
                floating_box.containing_block_rect_in_root_coordinate_space,
            );
        }
    }

    fn update_lowest_floating_descendant_bottom_margin_edge(&self) {
        let lowest = self
            .floats
            .borrow()
            .iter()
            .map(|floating_box| floating_box.margin_box_rect_in_root_coordinate_space.bottom())
            .max();
        self.lowest_floating_descendant_bottom_margin_edge.set(lowest);
    }

    fn translate_floats_in_subtree(&self, ancestor: Node, delta: FfiCssPixelPoint) {
        if (delta.x == CssPixels::default() && delta.y == CssPixels::default()) || !self.has_floating_boxes() {
            return;
        }
        let mut any_float_moved = false;
        for floating_box in self.floats.borrow_mut().iter_mut() {
            if !self.is_ancestor_of(ancestor, floating_box.box_) {
                continue;
            }
            floating_box.margin_box_rect_in_root_coordinate_space = floating_box
                .margin_box_rect_in_root_coordinate_space
                .translated(delta.x, delta.y);
            floating_box.containing_block_rect_in_root_coordinate_space = floating_box
                .containing_block_rect_in_root_coordinate_space
                .translated(delta.x, delta.y);
            any_float_moved = true;
        }
        if !any_float_moved {
            return;
        }
        self.update_lowest_floating_descendant_bottom_margin_edge();
        self.rebuild_float_bands();
    }

    fn border_box_left_of_box_avoiding_floats(
        &self,
        node: Node,
        metrics: &impl BoxMetricCells,
        space_used_by_floats: inline_formatting_context::SpaceUsedByFloats,
    ) -> CssPixels {
        if self.style(node).margin_left().is_auto() {
            return space_used_by_floats.left + metrics.margin_left();
        }
        if metrics.margin_left() >= CssPixels::default() {
            return space_used_by_floats.left.max(metrics.margin_left());
        }
        if space_used_by_floats.left > CssPixels::default() || space_used_by_floats.right > CssPixels::default() {
            return space_used_by_floats.left;
        }
        space_used_by_floats.left + metrics.margin_left()
    }

    fn avoid_float_intrusions(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        mut content_block_offset: CssPixels,
        containing_block_rect_in_root: CssPixelRect,
        probe: &mut FloatAvoidanceProbe,
    ) -> CssPixels {
        let node = sized_child.node;
        if !matches!(available_space.inline_size, AvailableSize::Definite(_))
            || !self.box_should_avoid_floats_because_it_establishes_fc(node)
        {
            return content_block_offset;
        }
        // https://drafts.csswg.org/css2/#floats
        // If necessary, implementations should clear the said element by placing it below any preceding floats, but may
        // place it adjacent to such floats if there is sufficient space.
        loop {
            let metrics = self.metrics_of(sized_child);
            let candidate_border_box_inline_size = probe
                .content_inline_size
                .unwrap_or_else(|| metrics.content_inline_size())
                + metrics.border_box_left(false)
                + metrics.border_box_right(false);
            let border_box_block_offset_in_root =
                containing_block_rect_in_root.y + content_block_offset - metrics.border_box_top(false);
            let band_rect = CssPixelRect {
                y: border_box_block_offset_in_root,
                height: metrics.border_box_block_size(false),
                ..containing_block_rect_in_root
            };
            let space = self.intrusions_for_band_into_rect(
                self.merged_band(border_box_block_offset_in_root, band_rect.bottom()),
                band_rect,
            );
            let constrained = space.left > CssPixels::default() || space.right > CssPixels::default();
            let border_box_left = self.border_box_left_of_box_avoiding_floats(node, &metrics, space);
            let mut must_clear = constrained
                && border_box_left + candidate_border_box_inline_size
                    > available_space.inline_size.to_px_or_zero() - space.right;
            if !must_clear {
                let border_rect = CssPixelRect {
                    x: band_rect.x + border_box_left,
                    y: border_box_block_offset_in_root,
                    width: candidate_border_box_inline_size,
                    height: metrics.border_box_block_size(false),
                };
                must_clear = self.floats.borrow().iter().any(|floating_box| {
                    let adjustment =
                        self.block_offset_adjustment_from_pending_ancestor_block_start_margins(floating_box.box_);
                    let margin_rect = floating_box
                        .margin_box_rect_in_root_coordinate_space
                        .translated(CssPixels::default(), adjustment);
                    // A zero-area overlap deliberately does not clear.
                    margin_rect.intersects(border_rect)
                });
            }
            if !must_clear {
                break;
            }
            let Some(next) = self.next_float_band_block_start_after(border_box_block_offset_in_root) else {
                break;
            };
            content_block_offset += next - border_box_block_offset_in_root;
            let position = FfiCssPixelPoint {
                x: containing_block_rect_in_root.x,
                y: containing_block_rect_in_root.y + content_block_offset,
            };
            // Deliberately re-run inline sizing after every band descent,
            // without committing: the winning candidate is what the run
            // prelude reproduces from the float-avoidance directive.
            probe.opportunity = self.float_reduced_inline_opportunity(sized_child, available_space, position);
            probe.content_inline_size = self.resolve_root_inline_metrics_and_content_size(
                sized_child,
                available_space,
                constraints,
                probe.opportunity,
            );
        }
        content_block_offset
    }

    pub(crate) fn block_offset_adjustment_from_pending_ancestor_block_start_margins(&self, node: Node) -> CssPixels {
        let margin_state = self.margin_state.borrow();
        let mut adjustment = CssPixels::default();
        for group in margin_state.pending_top_margin_groups() {
            if group.pinned_by_clearance {
                continue;
            }
            if self.is_inclusive_ancestor_of(group.box_, node) {
                adjustment += group.collapsed_margin - group.collapsed_margin_at_open;
            }
        }
        adjustment
    }

    /// Whether margins collapse through the box `sized_child` sizes, a box without a formatting context of its own that
    /// this flow laid out, where `children` sums up the children laid out in its flow.
    fn margins_collapse_through(&self, sized_child: ChildSizedHere<'_>, children: Option<&ChildrenSummary>) -> bool {
        let node = sized_child.node;
        // AD-HOC: The "and no in-flow children" of the margin collapsing rules is wrong, so margins collapse through a
        //         box with in-flow children where they collapse through each of them.
        //         (see https://github.com/w3c/csswg-drafts/pull/13699 )
        margins_can_collapse_through_box_itself(self.facts(node), self.style(node), || {
            self.cells_laid_out_here(sized_child).border_box_block_size(false)
        }) && children.is_none_or(|children| !children.has_child_whose_margins_do_not_collapse_through)
    }

    pub(crate) fn commit_pending_margin_before_inline_content(&self) -> CssPixels {
        let mut margin_state = self.margin_state.borrow_mut();
        let has_open = margin_state.has_open_top_margin_group();
        let collapsed = margin_state.current_collapsed_margin();
        margin_state.update_open_top_margin_group();
        margin_state.reset();
        if has_open { CssPixels::default() } else { collapsed }
    }

    pub(crate) fn reset_margin_state(&self) {
        self.margin_state.borrow_mut().reset();
    }

    pub(crate) fn has_floating_boxes(&self) -> bool {
        !self.floats.borrow().is_empty()
    }

    pub(crate) fn floating_box_count(&self) -> usize {
        self.floats.borrow().len()
    }

    pub(crate) fn take_floating_boxes_from(&self, index: usize) -> Vec<FloatingBox> {
        if self.floating_box_count() == index {
            return Vec::new();
        }
        let floating_boxes = self.floats.borrow_mut().split_off(index);
        self.update_lowest_floating_descendant_bottom_margin_edge();
        self.rebuild_float_bands();
        floating_boxes
    }

    pub(crate) fn append_floating_boxes(&self, floating_boxes: Vec<FloatingBox>) {
        if floating_boxes.is_empty() {
            return;
        }
        self.floats.borrow_mut().extend(floating_boxes);
        self.update_lowest_floating_descendant_bottom_margin_edge();
        self.rebuild_float_bands();
    }

    pub(crate) fn clear_floating_boxes(
        &self,
        node: Node,
        inline_context: Option<&inline_formatting_context::InlineFormattingContext>,
        containing_block_position_in_root: FfiCssPixelPoint,
    ) -> bool {
        let style = self.style(node);
        let mut result = false;
        let clear_to = |clearance_block_offset_in_root: CssPixels, result: &mut bool| {
            if clearance_block_offset_in_root == CssPixels::default() {
                return;
            }
            // NOTE: Floating boxes are globally relevant within this BFC, *but* their offset coordinates
            //       are relative to their containing block.
            //       This means that we have to first convert to a root-space block offset before clearing,
            //       and then convert back to a local block offset when assigning the cleared offset to
            //       the `child_box` layout state.
            let clearance = clearance_block_offset_in_root
                - containing_block_position_in_root.y
                - self.block_offset_adjustment_from_pending_ancestor_block_start_margins(node);
            if let Some(inline_context) = inline_context {
                if clearance > inline_context.block_axis_float_clearance() {
                    *result = true;
                    inline_context.set_block_axis_float_clearance(clearance);
                }
            } else if clearance
                > self
                    .block_offset_of_current_block_container
                    .get()
                    .expect("a block container flow cursor is active")
            {
                *result = true;
                self.block_offset_of_current_block_container.set(Some(clearance));
            }
        };

        // FIXME: Honor writing-mode, direction and text-orientation.
        if matches!(style.clear(), clear::LEFT | clear::BOTH | clear::INLINE_START) {
            clear_to(self.lowest_left_margin_edge.get(), &mut result);
        }
        if matches!(style.clear(), clear::RIGHT | clear::BOTH | clear::INLINE_END) {
            clear_to(self.lowest_right_margin_edge.get(), &mut result);
        }
        result
    }

    fn compute_normal_flow_inline_offset(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        content_position_in_root: FfiCssPixelPoint,
        content_inline_size: CssPixels,
    ) -> CssPixels {
        let node = sized_child.node;
        let metrics = self.metrics_of(sized_child);
        let mut inline_offset = CssPixels::default();
        let mut available_inline_size_within_containing_block = available_space.inline_size.to_px_or_zero();
        if self.box_should_avoid_floats_because_it_establishes_fc(node) {
            let space = self.intrusion_by_floats_into_rect(
                CssPixelRect {
                    x: content_position_in_root.x,
                    y: content_position_in_root.y,
                    width: content_inline_size,
                    height: metrics.content_block_size(),
                }
                .into(),
                CssPixels::default(),
                metrics.content_block_size(),
            );
            available_inline_size_within_containing_block -= space.left + space.right;
            // Subtracting the left margin here because it is applied again when the margin box offset is added below.
            inline_offset = self.border_box_left_of_box_avoiding_floats(node, &metrics, space) - metrics.margin_left();
        }

        let containing_block = self.containing_block(node);
        let containing_text_align = self.style(containing_block).text_align();
        if containing_text_align == text_align::_LIBWEB_CENTER {
            inline_offset += available_inline_size_within_containing_block / 2 - content_inline_size / 2;
        } else if containing_text_align == text_align::_LIBWEB_RIGHT {
            // Subtracting the left margin here because left and right margins need to be swapped when aligning to the right
            inline_offset += available_inline_size_within_containing_block
                - content_inline_size
                - metrics.margin_left()
                - metrics.border_box_left(false);
        } else {
            inline_offset += metrics.margin_left() + metrics.border_box_left(false);
        }
        inline_offset
    }

    fn marker_centered_block_offset(marker_line_height: CssPixels, marker_block_size: CssPixels) -> CssPixels {
        ((marker_line_height - marker_block_size) / 2).max(CssPixels::default())
    }

    fn keep_marker_for_its_run(&self, list_item: BlockContainer, marker: Node) -> used_values::RootInputCells {
        let list_item_cells = list_item.cells;
        let marker_constraints = ContainingBlockConstraints {
            percentage_basis_inline_size: list_item_cells
                .has_definite_inline_size
                .then_some(list_item_cells.content_inline_size),
            percentage_basis_block_size: list_item_cells
                .has_definite_block_size
                .then_some(list_item_cells.content_block_size),
            ..ContainingBlockConstraints::default()
        };
        used_values::cells_before_sizing(&self.run.callbacks, marker, marker_constraints)
    }

    /// Lays out and places the outside marker of `list_item`, and returns the block end of the marker's border box in
    /// the list item.
    fn layout_list_item_marker(
        &self,
        run: &FormattingContextRun<'pass>,
        list_item: BlockContainer,
        inline_space_used_before_list_item_elements_formatted: inline_formatting_context::SpaceUsedByFloats,
        list_item_first_baseline: Option<CssPixels>,
    ) -> Option<CssPixels> {
        let marker = self.facts(list_item.node).list_item_marker();
        if marker.is_invalid() {
            return None;
        }
        let marker_facts = self.facts(marker);
        // https://drafts.csswg.org/css-lists-3/#valdef-list-style-position-inside
        // "No special effect. (The ::marker is an inline element at the start of the list item's contents.)"
        if marker_facts.list_marker_is_inside() {
            return None;
        }
        // Animations can make `float` or `position` apply to ::marker.
        if marker_facts.is_floating() || marker_facts.is_absolutely_positioned() {
            return None;
        }
        let mut marker_cells = self.keep_marker_for_its_run(list_item, marker);
        let marker_style = self.style(marker);
        let marker_constraints = ContainingBlockConstraints {
            percentage_basis_inline_size: Some(list_item.cells.content_inline_size),
            ..ContainingBlockConstraints::default()
        };
        let max_content_inline_size = self.sizing().calculate_max_content_inline_size(
            sizing_context::SizedBox::new(marker, &marker_cells),
            marker_constraints,
        );
        marker_cells.set_content_inline_size(max_content_inline_size);
        let inner_available_space = AvailableSpace {
            inline_size: AvailableSize::definite(max_content_inline_size),
            block_size: AvailableSize::Indefinite,
        };
        let formatting_context::ChildLayout {
            outcome: marker_outcome,
            fragment_arrival,
            laid_out_box: marker_box,
        } = formatting_context::layout_inside_child_with_input_cells(
            run,
            None,
            None,
            marker,
            LayoutInput {
                available_space: inner_available_space,
                containing_block_constraints: marker_constraints,
                sizing: RootSizingDirectives {
                    adopt_automatic_content_block_size: true,
                    ..RootSizingDirectives::default()
                },
                participation: ParticipationInParentFormattingContext::Item,
            },
            marker_cells,
            true,
            list_item.percentage_block_insets_have_definite_basis,
        );
        marker_outcome.report_percentage_block_size_dependency_of(run, marker);

        let marker_metrics = marker_box.cells;
        let marker_block_size = marker_metrics.content_block_size();
        let marker_inline_size = marker_metrics.content_inline_size();
        let list_item_style = self.style(list_item.node);
        let marker_inline_offset = if list_item_style.direction() == direction::LTR {
            inline_space_used_before_list_item_elements_formatted.left - marker_inline_size
        } else {
            list_item.cells.content_inline_size - inline_space_used_before_list_item_elements_formatted.right
        };
        let marker_block_offset = if let Some(list_item_first_baseline) = list_item_first_baseline
            && let Some(marker_first_baseline) = marker_box.baselines.first
        {
            list_item_first_baseline - marker_first_baseline
        } else {
            round_css_pixels(Self::marker_centered_block_offset(
                marker_style.line_height(),
                marker_block_size,
            ))
        };

        formatting_context::place_child(
            &self.run,
            marker,
            formatting_context::PlacedChild::of_laid_out_box(&marker_box),
            FfiCssPixelPoint {
                x: round_css_pixels(marker_inline_offset),
                y: marker_block_offset,
            },
            None,
            fragment_arrival,
        );
        Some(
            marker_block_offset
                + marker_block_size
                + marker_metrics.border_box_bottom(marker_metrics.uses_collapsing_borders_model()),
        )
    }

    /// Lays out the contents of `node`, a child with a formatting context of its own, and returns its result where a
    /// run of its own laid it out, the fragments its placement takes, and the child's box as its layout left it.
    fn layout_inside(
        &self,
        run: &FormattingContextRun<'pass>,
        sized_child: ChildSizedHere<'_>,
        block_container: BlockContainer,
        input: LayoutInput,
        force_independent_context_run: bool,
    ) -> (
        Option<formatting_context::ChildLayoutResult>,
        Option<fragment_tree::ChildFragmentArrival>,
        run_state::LaidOutChildBox,
    ) {
        let formatting_context::ChildLayout {
            outcome,
            fragment_arrival,
            laid_out_box,
        } = formatting_context::layout_inside_child_with_input_cells(
            run,
            Some(self),
            None,
            sized_child.node,
            input,
            self.metrics_of(sized_child),
            force_independent_context_run,
            block_container.percentage_block_insets_have_definite_basis,
        );
        outcome.report_percentage_block_size_dependency_of(run, sized_child.node);
        let child_layout = match outcome {
            ChildLayoutOutcome::Skipped { .. } => None,
            ChildLayoutOutcome::Created(child_layout) => Some(child_layout),
        };
        (child_layout, fragment_arrival, laid_out_box)
    }

    fn child_layout_input(
        &self,
        containing_block: BlockContainer,
        containing_input: LayoutInput,
        available_space: AvailableSpace,
    ) -> LayoutInput {
        let sizing = self.sizing();
        let containing_block_constraints = sizing.constraints_for_child_context(
            containing_block.node,
            &containing_block.cells,
            containing_input.containing_block_constraints,
        );
        let mut available_space = available_space;
        if self.facts(containing_block.node).is_anonymous_button_content_box()
            && let Some(block_size) = containing_block_constraints.percentage_basis_block_size
        {
            // NB: Percentage heights inside the anonymous content box use the button's height, including during
            //     intrinsic measurement of the content box itself.
            available_space.block_size = AvailableSize::definite(block_size);
        }
        LayoutInput {
            available_space,
            containing_block_constraints,
            sizing: RootSizingDirectives::default(),
            participation: ParticipationInParentFormattingContext::BlockLevel,
        }
    }

    /// Lays out and places `node`, and returns its outcome where the parent flow holds the box.
    #[allow(clippy::too_many_arguments)]
    fn layout_block_level_box(
        &self,
        run: &FormattingContextRun<'pass>,
        node: Node,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        interrupted_inline_content: Option<InterruptedInlineContent>,
    ) -> Option<BlockLevelBoxOutcome> {
        let laid_out = self.layout_and_place_block_level_box(
            run,
            node,
            block_container,
            input,
            containing_block_position_in_root,
            interrupted_inline_content,
        )?;
        let content_offset = laid_out.content_offset.unwrap_or_default();
        let laid_out_box = laid_out.laid_out_box;
        let cells = laid_out_box.cells;
        let content_baselines = laid_out_box.baselines;
        let baselines_in_containing_block = formatting_context::baselines_laid_out_child_gives_container(
            &self.run.callbacks,
            formatting_context::LaidOutChild {
                node,
                metrics: &cells,
                content_baselines,
                content_offset,
            },
        );
        Some(BlockLevelBoxOutcome {
            content_offset,
            is_placed: laid_out.content_offset.is_some(),
            cells,
            content_baselines,
            collapses_through: laid_out.collapses_through,
            is_invisible_for_line_clamp: laid_out.fragment_facts.is_invisible_for_line_clamp,
            can_break_inside: self.box_can_break_inside(node, laid_out.has_unbreakable_lines),
            broken_inside: laid_out.broken_inside,
            baselines_in_containing_block,
            margin_box_block_end: laid_out.margin_box_block_end,
            deferred_legend: laid_out.deferred_legend,
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn layout_and_place_block_level_box(
        &self,
        run: &FormattingContextRun<'pass>,
        node: Node,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        interrupted_inline_content: Option<InterruptedInlineContent>,
    ) -> Option<LaidOutBlockLevelBox> {
        let available_space = input.available_space;
        let facts = self.facts(node);
        let containing_line_box_fragment = interrupted_inline_content.and_then(|interrupted| interrupted.line);

        if facts.is_absolutely_positioned() {
            // The static position sits where the next in-flow box would be placed, so the
            // collapsed margin pending from preceding siblings applies to it as well.
            let pending_margin = self.margin_state.borrow().pending_margin_for_next_box();
            // NB: An originally-inline absolutely positioned box never reaches this path; the tree
            //     builder keeps out-of-flow boxes in inline context, where static position markers
            //     pin them at their exact flow position.
            self.register_contained_abspos_child(
                node,
                pending_margin
                    + self
                        .block_offset_of_current_block_container
                        .get()
                        .expect("a block container flow cursor is active"),
                block_container,
            );
            return None;
        }

        if facts.is_list_item_marker_box() && !facts.is_floating() {
            return None;
        }

        // NOTE: It is possible to encounter SVGMaskBox and SVGClipBox nodes while doing layout of the
        //       formatting context established by a <foreignObject> that references them. Skip them
        //       before sizing them; SVGFormattingContext lays them out on behalf of the referencing
        //       element.
        if facts.is_svg_mask_box() || facts.is_svg_clip_box() {
            return None;
        }

        let block_container_inline_size = block_container.cells.content_inline_size;
        // NB: A child with a formatting context of its own is sized in these cells ahead of its run, which takes them.
        //     A box without one is laid out in them.
        let cells_of_child = Cell::new(used_values::cells_before_sizing(
            &self.run.callbacks,
            node,
            input.containing_block_constraints,
        ));
        let sized_child = if formatting_context::formatting_context_type_created_by_box(facts).is_some() {
            ChildSizedHere::ahead_of_its_run(node, &cells_of_child)
        } else {
            ChildSizedHere::laid_out_in(node, &cells_of_child)
        };
        let _percentage_block_size_dependency_tracking = self
            .run
            .state
            .track_percentage_block_size_dependency_while_laying_out(node);
        let mut fragment_facts = run_state::FragmentFactsDecidedByParent::default();
        if self.laying_out_invisible_line_clamp_content.get() {
            fragment_facts.is_invisible_for_line_clamp = true;
        }
        if facts.is_anonymous_button_content_wrapper() {
            let only_for_button_content_alignment = block_container
                .cells
                .has_definite_block_size_only_for_button_content_alignment;
            self.update_cells_of(sized_child, |cells| {
                cells.has_definite_block_size_only_for_button_content_alignment = only_for_button_content_alignment;
            });
        }

        self.resolve_vertical_box_model_metrics(sized_child, block_container_inline_size);
        assert_eq!(self.containing_block(node), block_container.node);
        let continuation = self
            .run
            .fragmentainer
            .is_some()
            .then(|| {
                self.pending_continuation_token
                    .borrow_mut()
                    .take_if(|token| token.node == node)
            })
            .flatten();
        let fragments_inside = self.run.fragmentainer.is_some() && self.break_mode.get() != BreakMode::NoBreaks;
        let suppresses_fragmentation_inside = fragments_inside && facts.node_has_size_containment();
        let suppresses_unforced_breaks_inside = fragments_inside
            && !suppresses_fragmentation_inside
            && self.break_mode.get() == BreakMode::AllBreaks
            && fragmentation::avoids_column_break_inside(self.style(node).break_inside())
            && self.fragmentainer_has_content.get();
        let mut continuation_padding_top = CssPixels::default();
        if continuation.is_some() {
            continuation_padding_top = self.metrics_of(sized_child).padding_top();
            self.update_cells_of(sized_child, |cells| {
                cells.margin_top = CssPixels::default();
                cells.border_top = CssPixels::default();
                cells.padding_top = CssPixels::default();
            });
            fragment_facts.continues_from_previous_fragment = true;
        }

        if facts.is_floating() {
            let block_offset = self
                .block_offset_of_current_block_container
                .get()
                .expect("a block container flow cursor is active");
            let margin_top = self.margin_state.borrow().pending_margin_for_next_box();
            let (_, float_handed_back_by_its_run, float_content_offset) = self.layout_floating_box(
                run,
                sized_child,
                block_container,
                input,
                containing_block_position_in_root,
                margin_top + block_offset,
                None,
                fragment_facts,
            );
            return Some(LaidOutBlockLevelBox {
                content_offset: float_content_offset,
                margin_box_block_end: self
                    .floats
                    .borrow()
                    .last()
                    .map(|floating_box| floating_box.bottom_margin_edge),
                ..LaidOutBlockLevelBox::of_box(
                    float_handed_back_by_its_run
                        .expect("a floating box of a block formatting context is laid out by a run of its own"),
                    fragment_facts,
                )
            });
        }

        let in_flow_reuse = match self.probe_in_flow_reuse(
            node,
            block_container.node,
            block_container_inline_size,
            input,
            containing_line_box_fragment,
        ) {
            InFlowReuse::Hit { entry, fragment } => {
                let (content_offset, margin_box_block_end) =
                    self.reuse_in_flow_child(sized_child, block_container, &entry, fragment);
                return Some(LaidOutBlockLevelBox {
                    content_offset: Some(content_offset),
                    margin_box_block_end: Some(margin_box_block_end),
                    ..LaidOutBlockLevelBox::of_box(
                        Self::laid_out_box_of_box_laid_out_here(sized_child, entry.baselines),
                        fragment_facts,
                    )
                });
            }
            attempt => attempt,
        };

        let is_in_flow_candidate = self.run.fragmentainer.is_some() && self.is_first_or_last_in_flow_candidate(node);
        let discards_block_start_margin =
            is_in_flow_candidate && self.discard_block_start_margin_of_next_child.replace(false);
        if !discards_block_start_margin {
            self.margin_state
                .borrow_mut()
                .add_margin(self.metrics_of(sized_child).margin_top());
        }
        let introduced_clearance = self.clear_floating_boxes(node, None, containing_block_position_in_root);
        if introduced_clearance {
            self.margin_state.borrow_mut().reset();
        }
        self.margin_state.borrow_mut().update_open_top_margin_group();

        let block_offset = self
            .block_offset_of_current_block_container
            .get()
            .expect("a block container flow cursor is active");

        let style = self.style(node);
        let box_is_html_element_in_quirks_mode = facts.document_in_quirks_mode()
            && facts.is_html_html_element()
            && crate::layout::sizing_context::is_plain_auto(style.height());
        let block_size_is_definite_from_aspect_ratio = self.metrics_of(sized_child).has_definite_inline_size()
            && facts.has_preferred_aspect_ratio()
            && self.sizing().box_is_sized_as_replaced_element(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space,
                input.containing_block_constraints,
            );

        // NOTE: In quirks mode, the html element's block size matches the viewport so it can be treated as definite.
        if self.metrics_of(sized_child).has_definite_block_size()
            || box_is_html_element_in_quirks_mode
            || block_size_is_definite_from_aspect_ratio
        {
            self.resolve_used_block_size_if_treated_as_auto(
                sized_child,
                available_space,
                input.containing_block_constraints,
                None,
                None,
            );
        }

        let independent_type = formatting_context::formatting_context_type_created_by_box(facts);
        let has_independent_formatting_context = independent_type.is_some();

        if !has_independent_formatting_context && !facts.is_block_container() {
            // Keep the C++ behavior: diagnose and skip this unsupported box after its
            // vertical metrics have been resolved.
            eprintln!("FIXME: Block-level box is not BlockContainer but does not create formatting context");
            formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
                run.state,
                &self.run.callbacks,
                node,
                self.sizing().resolve_percentage_block_size_dependency(node),
            );
            return Some(LaidOutBlockLevelBox {
                collapses_through: self.margins_collapse_through(sized_child, None),
                ..LaidOutBlockLevelBox::of_box(
                    Self::laid_out_box_of_box_laid_out_here(sized_child, DerivedBaselines::default()),
                    fragment_facts,
                )
            });
        }
        // If first child margin top will collapse with margin-top of containing block then margin-top of child is 0
        let margin_top = self.margin_state.borrow().pending_margin_for_next_box();

        let box_opens_top_margin_group = !has_independent_formatting_context
            && continuation.is_none()
            && self.cells_laid_out_here(sized_child).border_top() == CssPixels::default()
            && self.cells_laid_out_here(sized_child).padding_top() == CssPixels::default()
            && !self.margin_state.borrow().has_open_top_margin_group();

        let container_facts = self.facts(block_container.node);
        let box_is_positioned_by_fieldset_layout =
            container_facts.is_fieldset_box() && container_facts.rendered_legend() == node;

        // Earlier sibling placement may have invalidated cached float bands.
        self.rebuild_float_bands();

        let mut content_block_offset = block_offset + margin_top + self.metrics_of(sized_child).border_box_top(false);
        let containing_block_rect_in_root =
            self.containing_block_rect(block_container, containing_block_position_in_root);
        let containing_block_rect_in_root_now = containing_block_rect_in_root.translated(
            CssPixels::default(),
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(node),
        );
        let content_position_in_root_now = |content_block_offset: CssPixels| FfiCssPixelPoint {
            x: containing_block_rect_in_root_now.x,
            y: containing_block_rect_in_root_now.y + content_block_offset,
        };

        let opportunity = self.float_reduced_inline_opportunity(
            sized_child,
            available_space,
            content_position_in_root_now(content_block_offset),
        );
        let mut probe = FloatAvoidanceProbe {
            opportunity,
            content_inline_size: self.resolve_root_inline_metrics_and_content_size(
                sized_child,
                available_space,
                input.containing_block_constraints,
                opportunity,
            ),
        };
        content_block_offset = self.avoid_float_intrusions(
            sized_child,
            available_space,
            input.containing_block_constraints,
            content_block_offset,
            containing_block_rect_in_root_now,
            &mut probe,
        );
        let float_avoidance_inline_size = probe.opportunity;
        if !has_independent_formatting_context && let Some(content_inline_size) = probe.content_inline_size {
            self.cells_laid_out_here(sized_child)
                .set_content_inline_size(content_inline_size);
        }
        let content_inline_size_now = probe
            .content_inline_size
            .unwrap_or_else(|| self.metrics_of(sized_child).content_inline_size());
        let content_inline_offset = self.compute_normal_flow_inline_offset(
            sized_child,
            available_space,
            content_position_in_root_now(content_block_offset),
            content_inline_size_now,
        );

        let is_list_item_box = facts.is_list_item_box();
        let marker = facts.list_item_marker();

        let is_table_formatting_context = independent_type == Some(formatting_context::FormattingContextType::Table);
        let mut pending_position = None;
        let mut legend_flow_position = None;
        if box_is_positioned_by_fieldset_layout {
            legend_flow_position = Some(geometry::LogicalOffset {
                inline_offset: content_inline_offset,
                block_offset: content_block_offset,
            });
        } else if !box_opens_top_margin_group {
            pending_position = Some(FfiCssPixelPoint {
                x: content_inline_offset,
                y: content_block_offset,
            });
        }

        let available_space_for_block_size_resolution = self.sizing().available_space_for_block_size_resolution(
            node,
            available_space,
            input.containing_block_constraints,
        );

        // Whether a block size is treated as automatic can depend on the
        // inline size being definite (aspect-ratio transfer), which an
        // independent run only commits in its prelude — so independent
        // children resolve these pre-body block sizes there instead.
        if !has_independent_formatting_context {
            self.resolve_used_block_size_if_not_treated_as_auto(
                sized_child,
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
            );
            // NOTE: Flex containers with an automatic block size are treated as max-content, so resolve it early.
            if facts.has_auto_content_box_size() || style.display().is_flex_inside() {
                self.resolve_used_block_size_if_treated_as_auto(
                    sized_child,
                    available_space_for_block_size_resolution,
                    input.containing_block_constraints,
                    None,
                    None,
                );
            }
        }

        let continuation_inner_size_excess = if continuation.is_some()
            && (if block_size_is_definite_from_aspect_ratio && style.height().is_auto() {
                style.box_sizing_for_aspect_ratio()
            } else {
                style.box_sizing()
            }) == box_sizing::BORDER_BOX
        {
            continuation_padding_top
        } else {
            CssPixels::default()
        };
        if continuation.is_some() && self.cells_laid_out_here(sized_child).has_definite_block_size() {
            let used = self.cells_laid_out_here(sized_child);
            used.set_content_block_size(
                (used.content_block_size() - continuation_inner_size_excess).max(CssPixels::default()),
            );
        }

        // Before we insert the children of a list item we need to know the location of the marker.
        // If we do not do this then left-floating elements inside the list item will push the marker to the right,
        // in some cases even causing it to overlap with the non-floating content of the list.
        let mut inline_space_used_before_children_formatted = inline_formatting_context::SpaceUsedByFloats::default();
        if is_list_item_box && !marker.is_invalid() {
            let marker_facts = self.facts(marker);
            if !marker_facts.list_marker_is_inside() {
                let marker_style = self.style(marker);
                let estimated_block_size = line_builder::normal_line_height(marker_style);
                let marker_block_offset =
                    Self::marker_centered_block_offset(marker_style.line_height(), estimated_block_size);
                let list_item_block_size = self.metrics_of(sized_child).content_block_size();
                inline_space_used_before_children_formatted = self.intrusion_by_floats_into_rect(
                    CssPixelRect {
                        x: content_position_in_root_now(content_block_offset).x + content_inline_offset,
                        y: content_position_in_root_now(content_block_offset).y,
                        width: content_inline_size_now,
                        height: list_item_block_size,
                    }
                    .into(),
                    marker_block_offset,
                    marker_block_offset,
                );
            }
        }

        let break_mode_outside = self.break_mode.get();
        if suppresses_fragmentation_inside {
            self.break_mode.set(BreakMode::NoBreaks);
        } else if suppresses_unforced_breaks_inside {
            self.break_mode.set(BreakMode::ForcedBreaksOnly);
        }
        let mut baselines_of_contents_laid_out_in_this_flow = None;
        let mut summary_of_children_laid_out_in_this_flow = None;
        let mut fragment_arrival_of_child_with_own_formatting_context = None;
        // NB: A child its own run laid out is placed as that run handed it back, and any other with its cells here.
        let mut child_handed_back_by_its_run = None;
        let mut laid_out_box_handed_back_by_its_run = None;
        if has_independent_formatting_context {
            // Margins of elements that establish new formatting contexts do not collapse with their in-flow children
            self.margin_state.borrow_mut().reset();

            let definite_content_block_size_of_fieldset_to_fill = (container_facts.is_fieldset_box()
                && node_facts::node_is_fieldset_content_wrapper(facts.data(), facts.computed_values_view_if_styled()))
            .then_some(input.containing_block_constraints.percentage_basis_block_size)
            .flatten();
            let inside_layout_input = LayoutInput {
                available_space,
                containing_block_constraints: input.containing_block_constraints,
                sizing: RootSizingDirectives {
                    forced_content_block_size: definite_content_block_size_of_fieldset_to_fill.map(
                        |fieldset_content_block_size| {
                            let metrics = self.metrics_of(sized_child);
                            (fieldset_content_block_size
                                - content_block_offset
                                - metrics.border_box_bottom(false)
                                - metrics.margin_bottom())
                            .max(CssPixels::default())
                        },
                    ),
                    forced_min_border_box_block_size: if is_table_formatting_context {
                        input.sizing.forced_min_border_box_block_size
                    } else {
                        None
                    },
                    block_parent_resolved_content_inline_size: probe.content_inline_size,
                    table_box_content_block_offset_in_wrapper: is_table_formatting_context
                        .then_some(content_block_offset),
                    float_avoidance_inline_size,
                    outer_float_intrusion_before_list_item_children: inline_space_used_before_children_formatted,
                    ..RootSizingDirectives::default()
                },
                participation: ParticipationInParentFormattingContext::BlockLevel,
            };
            let pre_run_table_border_box = is_table_formatting_context.then(|| {
                let metrics = self.metrics_of(sized_child);
                (metrics.border_box_left(false), metrics.border_box_top(false))
            });
            let (child_layout, fragment_arrival, laid_out_box) =
                self.layout_inside(run, sized_child, block_container, inside_layout_input, true);
            assert!(
                child_layout.is_some(),
                "a block-level box with a formatting context of its own is laid out by its run"
            );
            fragment_arrival_of_child_with_own_formatting_context = fragment_arrival;
            child_handed_back_by_its_run = Some(formatting_context::PlacedChild::of_laid_out_box(&laid_out_box));
            let table_box = laid_out_box.cells;
            laid_out_box_handed_back_by_its_run = Some(laid_out_box);
            if container_facts.is_table_wrapper() && style.display().is_table_inside() {
                self.table_box_in_wrapper_border_box_inline_size.set(Some(
                    table_box.border_box_inline_size(table_box.uses_collapsing_borders_model()),
                ));
                self.table_box_in_wrapper_border_box_block_size.set(Some(
                    table_box.border_box_block_size(table_box.uses_collapsing_borders_model()),
                ));
                if table_box.uses_collapsing_borders_model()
                    && let Some(position) = pending_position.as_mut()
                    && let Some((pre_run_border_box_left, pre_run_border_box_top)) = pre_run_table_border_box
                {
                    position.x += table_box.border_box_left(true) - pre_run_border_box_left;
                    position.y += table_box.border_box_top(true) - pre_run_border_box_top;
                }
            }
        } else {
            // This box participates in the current block container's flow.
            let space_available_for_children = if facts.is_anonymous() {
                available_space
            } else {
                self.cells_laid_out_here(sized_child)
                    .available_inner_space_or_constraints_from(available_space)
            };
            if self.cells_laid_out_here(sized_child).border_top() > CssPixels::default()
                || self.cells_laid_out_here(sized_child).padding_top() > CssPixels::default()
            {
                // margin-top of block container can't collapse with its children if it has non-zero border or padding.
                self.margin_state.borrow_mut().reset();
            } else if box_opens_top_margin_group {
                self.margin_state
                    .borrow_mut()
                    .open_top_margin_group(node, introduced_clearance);
            }
            if is_in_flow_candidate {
                self.discard_block_start_margin_of_next_child.set(
                    discards_block_start_margin
                        && !facts.children_are_inline()
                        && self.cells_laid_out_here(sized_child).border_top() == CssPixels::default()
                        && self.cells_laid_out_here(sized_child).padding_top() == CssPixels::default(),
                );
            }
            let content_box_position_in_root = FfiCssPixelPoint {
                x: containing_block_position_in_root.x + content_inline_offset,
                y: containing_block_position_in_root.y + content_block_offset,
            };
            self.incoming_container_token.replace(continuation.clone());
            let node_as_block_container =
                BlockContainer::laid_out_in(&self.run.callbacks, node, self.metrics_of(sized_child), block_container);
            let children_summary = if facts.children_are_inline() {
                self.layout_inline_children(
                    run,
                    node_as_block_container,
                    input,
                    content_box_position_in_root,
                    space_available_for_children,
                )
            } else {
                self.layout_block_level_children(
                    run,
                    node_as_block_container,
                    input,
                    content_box_position_in_root,
                    space_available_for_children,
                )
            };
            baselines_of_contents_laid_out_in_this_flow = Some(children_summary.baselines);
            summary_of_children_laid_out_in_this_flow = Some(children_summary);
            if is_in_flow_candidate {
                self.discard_block_start_margin_of_next_child.set(false);
            }
            if box_opens_top_margin_group {
                let resolved_margin_top = self.margin_state.borrow_mut().take_pending_top_margin();
                let final_content_block_offset = if introduced_clearance {
                    content_block_offset
                } else {
                    block_offset + resolved_margin_top + self.cells_laid_out_here(sized_child).border_box_top(false)
                };
                if box_is_positioned_by_fieldset_layout {
                    legend_flow_position = Some(geometry::LogicalOffset {
                        inline_offset: content_inline_offset,
                        block_offset: final_content_block_offset,
                    });
                } else {
                    pending_position = Some(FfiCssPixelPoint {
                        x: content_inline_offset,
                        y: final_content_block_offset,
                    });
                }
                self.translate_floats_in_subtree(
                    node,
                    FfiCssPixelPoint {
                        x: CssPixels::default(),
                        y: final_content_block_offset - content_block_offset,
                    },
                );
            }
        }

        // An independent run resolved its automatic block size in its own epilogue; same-flow children still resolve
        // here. Tables set their block size during their run in every case.
        if !has_independent_formatting_context && !style.display().is_table_inside() {
            self.resolve_used_block_size_if_treated_as_auto(
                sized_child,
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
                None,
                summary_of_children_laid_out_in_this_flow.as_ref(),
            );
        }
        if !has_independent_formatting_context
            && self.sizing().block_size_is_ratio_dependent(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
            )
        {
            let content_block_size = self.compute_automatic_block_size_for_block_level_element(
                sized_child,
                self.cells_laid_out_here(sized_child)
                    .available_inner_space_or_constraints_from(available_space_for_block_size_resolution),
                input.containing_block_constraints,
                None,
                summary_of_children_laid_out_in_this_flow.as_ref(),
            );
            self.sizing().apply_automatic_minimum_block_size_from_aspect_ratio(
                self.sized_cells_of(sized_child),
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
                Some(content_block_size),
            );
        }

        if let Some(continuation) = &continuation {
            self.size_continuation(
                sized_child,
                continuation.consumed_block_size,
                continuation_inner_size_excess,
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
                summary_of_children_laid_out_in_this_flow.as_ref(),
            );
        }

        let has_unbreakable_lines = summary_of_children_laid_out_in_this_flow
            .as_ref()
            .is_some_and(ChildrenSummary::has_unbreakable_lines);
        let mut broken_token_of_box = None;
        if let Some(fragmentainer) = self.run.fragmentainer.as_deref() {
            let content_block_start_in_root = pending_position.map(|position| {
                containing_block_position_in_root.y
                    + self.block_offset_adjustment_from_pending_ancestor_block_start_margins(node)
                    + position.y
            });
            let stop_token_of_children = summary_of_children_laid_out_in_this_flow
                .as_mut()
                .and_then(|summary| summary.fragmentainer_stop_token.take());
            let children = summary_of_children_laid_out_in_this_flow.as_ref();
            let broken_token = stop_token_of_children.or_else(|| {
                if !self.allows_unforced_breaks() {
                    return None;
                }
                let block_size = fragmentainer.block_size?;
                let content_block_start_in_root = content_block_start_in_root?;
                let can_break_after_block_start_edges = self.box_can_break_inside(node, has_unbreakable_lines)
                    && content_block_start_in_root - self.cells_laid_out_here(sized_child).border_box_top(false)
                        < block_size;
                if can_break_after_block_start_edges && content_block_start_in_root > block_size {
                    fragmentainer.note_space_shortage(content_block_start_in_root);
                }
                let breaks_inside = can_break_after_block_start_edges
                    && content_block_start_in_root <= block_size
                    && content_block_start_in_root + self.cells_laid_out_here(sized_child).content_block_size()
                        > block_size
                    && content_block_start_in_root
                        + children
                            .map_or(CssPixels::default(), ChildrenSummary::block_end_of_content)
                            .max(self.unbreakable_content_floor(node, children))
                        <= block_size;
                breaks_inside.then(|| {
                    std::rc::Rc::new(fragmentation::BlockBreakToken {
                        node,
                        consumed_block_size: CssPixels::default(),
                        resume: fragmentation::BlockResume::AfterChildren,
                    })
                })
            });
            if let Some(mut token) = broken_token {
                let used = self.cells_laid_out_here(sized_child);
                if let (Some(block_size), Some(content_block_start_in_root)) =
                    (fragmentainer.block_size, content_block_start_in_root)
                {
                    let content_block_size = used.content_block_size();
                    let remaining_block_size = (block_size - content_block_start_in_root).max(CssPixels::default());
                    fragmentainer.note_space_shortage(content_block_start_in_root + content_block_size);
                    used.set_content_block_size(if used.has_definite_block_size() {
                        content_block_size.min(remaining_block_size)
                    } else {
                        remaining_block_size
                    });
                }
                used.update_cells(|cells| {
                    cells.padding_bottom = CssPixels::default();
                    cells.border_bottom = CssPixels::default();
                    cells.margin_bottom = CssPixels::default();
                });
                fragment_facts.continues_in_next_fragment = true;
                std::rc::Rc::make_mut(&mut token).consumed_block_size = continuation
                    .as_ref()
                    .map_or(CssPixels::default(), |continuation| continuation.consumed_block_size)
                    + used.content_block_size();
                broken_token_of_box = Some(token);
            }
        }

        self.break_mode.set(break_mode_outside);

        // Now that our children are formatted we place the ListItemBox with the left space we remembered.
        if is_list_item_box && !has_independent_formatting_context && continuation.is_none() {
            let list_item_first_baseline = baselines_of_contents_laid_out_in_this_flow
                .expect("a list item in this flow had its contents laid out")
                .first;
            let marker_border_box_block_end = self.layout_list_item_marker(
                run,
                BlockContainer::laid_out_in(&self.run.callbacks, node, self.metrics_of(sized_child), block_container),
                inline_space_used_before_children_formatted,
                list_item_first_baseline,
            );
            if let Some(summary) = summary_of_children_laid_out_in_this_flow.as_mut()
                && let Some(marker_border_box_block_end) = marker_border_box_block_end
            {
                if summary.includes_list_item_marker {
                    summary.include_placed_child(marker_border_box_block_end);
                }
                // NB: The laid-out marker has a formatting context of its own, so margins do not collapse through it.
                if self.run.callbacks.parent(marker) == node {
                    summary.has_child_whose_margins_do_not_collapse_through = true;
                }
            }
        }

        let dependency_was_not_reported_by_a_child_run = !has_independent_formatting_context;
        let mut depends_on_percentage_block_size = false;
        if dependency_was_not_reported_by_a_child_run {
            depends_on_percentage_block_size = self.sizing().resolve_percentage_block_size_dependency(node);
            formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
                run.state,
                &self.run.callbacks,
                node,
                depends_on_percentage_block_size,
            );
        }

        let insets = self.compute_inset(run, node, block_container);

        let lines_laid_out_in_this_flow = summary_of_children_laid_out_in_this_flow
            .as_mut()
            .and_then(|summary| summary.lines.take());
        let placed_child = child_handed_back_by_its_run
            .unwrap_or_else(|| {
                formatting_context::PlacedChild::laid_out_here(
                    self.metrics_of(sized_child),
                    lines_laid_out_in_this_flow,
                    &self.run,
                )
            })
            .with_in_flow_box_insets(insets)
            .with_fragment_facts(fragment_facts)
            .with_offset_from_relatively_positioned_inline_ancestors(
                interrupted_inline_content.map_or(FfiCssPixelPoint::default(), |interrupted| {
                    interrupted.offset_from_relatively_positioned_inline_ancestors
                }),
            );
        let mut deferred_legend = None;
        let mut placed = match pending_position {
            Some(position) => formatting_context::place_child(
                &self.run,
                node,
                placed_child,
                position,
                containing_line_box_fragment,
                fragment_arrival_of_child_with_own_formatting_context,
            ),
            None => {
                deferred_legend = Some(Box::new(DeferredLegend {
                    flow_position: legend_flow_position.expect("only a rendered legend waits for its position"),
                    placed_child,
                    fragment_arrival: fragment_arrival_of_child_with_own_formatting_context,
                }));
                None
            }
        };
        if let Some(summary) = summary_of_children_laid_out_in_this_flow.as_mut()
            && let Some(placed) = placed.as_mut()
        {
            for abspos_child in std::mem::take(&mut placed.abspos_boxes_placed_with_it) {
                if self.run.callbacks.parent(abspos_child.box_) == node {
                    summary.include_placed_child(abspos_child.border_box_block_end);
                }
            }
        }

        let collapses_through = !has_independent_formatting_context
            && self.margins_collapse_through(sized_child, summary_of_children_laid_out_in_this_flow.as_ref());
        // NB: A child its own run laid out has the metrics that run handed back, and any other those of its cells here.
        let metrics = match &laid_out_box_handed_back_by_its_run {
            Some(handed_back) => handed_back.cells,
            None => self.metrics_of(sized_child),
        };
        if !collapses_through {
            let mut margin_state = self.margin_state.borrow_mut();
            if !margin_state.box_last_in_flow_child_margin_bottom_collapsed || fragment_facts.continues_in_next_fragment
            {
                margin_state.reset();
            }
            drop(margin_state);
            self.block_offset_of_current_block_container.set(Some(
                pending_position.unwrap_or_default().y
                    + metrics.content_block_size()
                    + metrics.border_box_bottom(metrics.uses_collapsing_borders_model()),
            ));
        }
        self.margin_state
            .borrow_mut()
            .box_last_in_flow_child_margin_bottom_collapsed = false;
        self.margin_state.borrow_mut().add_margin(metrics.margin_bottom());
        self.margin_state.borrow_mut().update_open_top_margin_group();

        let content_offset = pending_position.unwrap_or_default();
        let margin_box_block_end = content_offset.y
            + metrics.content_block_size()
            + metrics.margin_box_bottom(metrics.uses_collapsing_borders_model());
        self.conclude_in_flow_reuse(
            in_flow_reuse,
            sized_child,
            baselines_of_contents_laid_out_in_this_flow.unwrap_or_default(),
            placed.map(|placed| (content_offset, placed)),
            depends_on_percentage_block_size,
        );
        let broken_inside = broken_token_of_box.map(|token| BrokenInside {
            token,
            block_end_of_content: summary_of_children_laid_out_in_this_flow
                .as_ref()
                .expect("a box breaks inside the children laid out in this flow")
                .block_end_of_content(),
        });
        Some(LaidOutBlockLevelBox {
            broken_inside,
            has_unbreakable_lines,
            collapses_through,
            laid_out_box: laid_out_box_handed_back_by_its_run.unwrap_or_else(|| {
                Self::laid_out_box_of_box_laid_out_here(
                    sized_child,
                    baselines_of_contents_laid_out_in_this_flow.unwrap_or_default(),
                )
            }),
            fragment_facts,
            content_offset: pending_position,
            margin_box_block_end: Some(margin_box_block_end),
            deferred_legend,
        })
    }

    /// The box `child` as this flow laid it out in cells its layout owns, with the `baselines` of its contents.
    fn laid_out_box_of_box_laid_out_here(
        child: ChildSizedHere<'_>,
        baselines: DerivedBaselines,
    ) -> run_state::LaidOutChildBox {
        run_state::LaidOutChildBox::sized_here(
            child
                .cells_laid_out_here()
                .expect("a box laid out in this flow is laid out in cells its layout owns")
                .get(),
            baselines,
        )
    }

    /// Whether the layout of an in-flow child can be stored and reused by `InFlowBlockKey`: only a block container
    /// laid out in this run's own flow, where no float and no line clamp can read its position in the root.
    fn in_flow_child_may_be_reused(
        &self,
        node: Node,
        block_container: Node,
        containing_line_box_fragment: Option<used_values::LineBoxFragmentCoordinate>,
    ) -> bool {
        let facts = self.facts(node);
        let container_facts = self.facts(block_container);
        !self.run.callbacks.reads_no_memoized_layout()
            && self.run.fragmentainer.is_none()
            && self.run.fragments.is_some()
            && !self.run.should_collect_devtools_layout_data
            && containing_line_box_fragment.is_none()
            && self.floats.borrow().is_empty()
            && !self.is_line_clamp_container
            && self.max_lines.get().is_none()
            && !self.laying_out_invisible_line_clamp_content.get()
            && formatting_context::formatting_context_type_created_by_box(facts).is_none()
            && facts.is_block_container()
            && !facts.is_anonymous_button_content_wrapper()
            && !facts.has_preferred_aspect_ratio()
            && self.style(node).clear() == clear::NONE
            && !container_facts.is_fieldset_box()
            && !container_facts.is_table_wrapper()
    }

    fn probe_in_flow_reuse(
        &self,
        node: Node,
        block_container: Node,
        block_container_inline_size: CssPixels,
        input: LayoutInput,
        containing_line_box_fragment: Option<used_values::LineBoxFragmentCoordinate>,
    ) -> InFlowReuse {
        if !self.in_flow_child_may_be_reused(node, block_container, containing_line_box_fragment) {
            return InFlowReuse::Ineligible;
        }
        let callbacks = &self.run.callbacks;
        let key = in_flow_block_cache::InFlowBlockKey::new(
            &self.run,
            input,
            block_container_inline_size,
            self.style(block_container).direction() == direction::RTL,
            self.margin_state.borrow().clone(),
        );
        let validity = fc_run_cache::run_root_validity(callbacks, node);
        // NB: The committed subtree is reused only while it is the one the stored layout produced.
        let hit = callbacks
            .arena()
            .with_current_committed_fragment(node, |fragment| fragment.clone())
            .and_then(|fragment| {
                let entry = callbacks.in_flow_block_cache_store().matching(
                    node.slot_index(),
                    validity,
                    &key,
                    fragment.identity,
                )?;
                Some((entry, fragment))
            });
        if let Some((entry, fragment)) = hit {
            return InFlowReuse::Hit { entry, fragment };
        }
        InFlowReuse::Store {
            key,
            validity,
            flow_position: self.flow_position(),
            abspos_containing_block_info_contributions_received: self
                .abspos_containing_block_info_contributions_received(),
        }
    }

    fn flow_position(&self) -> CssPixels {
        self.block_offset_of_current_block_container
            .get()
            .expect("a block container flow cursor is active")
    }

    fn abspos_containing_block_info_contributions_received(&self) -> u64 {
        self.run.fragments.as_deref().map_or(0, |fragments| {
            fragments.abspos_containing_block_info_contributions_received()
        })
    }

    fn reuse_in_flow_child(
        &self,
        sized_child: ChildSizedHere<'_>,
        block_container: BlockContainer,
        entry: &in_flow_block_cache::InFlowBlockEntry,
        fragment: std::sync::Arc<fragment_tree::Fragment>,
    ) -> (FfiCssPixelPoint, CssPixels) {
        let node = sized_child.node;
        self.run.callbacks.layout_trace().reused_in_flow_subtree(node);
        self.cells_laid_out_here(sized_child).set(entry.cells);
        let flow_position = self.flow_position();
        *self.margin_state.borrow_mut() = entry.outgoing_margins.clone();
        formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
            self.run.state,
            &self.run.callbacks,
            node,
            entry.depends_on_percentage_block_size,
        );
        let insets = self.compute_inset(&self.run, node, block_container);
        let content_offset = FfiCssPixelPoint {
            x: entry.offset_from_flow_position.x,
            y: entry.offset_from_flow_position.y + flow_position,
        };
        formatting_context::place_child(
            &self.run,
            node,
            formatting_context::PlacedChild::new(fragment_tree::FragmentGeometry {
                cells: self.metrics_of(sized_child),
                line_data: None,
            })
            .with_in_flow_box_insets(insets),
            content_offset,
            None,
            Some(fragment_tree::ChildFragmentArrival::CommittedFragment {
                fragment,
                reuses_subtree: true,
            }),
        );
        self.block_offset_of_current_block_container
            .set(Some(flow_position + entry.flow_position_advance));
        (
            content_offset,
            flow_position + entry.margin_box_bottom_from_flow_position,
        )
    }

    fn conclude_in_flow_reuse(
        &self,
        attempt: InFlowReuse,
        sized_child: ChildSizedHere<'_>,
        baselines: DerivedBaselines,
        placed: Option<(FfiCssPixelPoint, fragment_tree::PlacedFragment)>,
        depends_on_percentage_block_size: bool,
    ) {
        let node = sized_child.node;
        let InFlowReuse::Store {
            key,
            validity,
            flow_position,
            abspos_containing_block_info_contributions_received,
        } = attempt
        else {
            return;
        };
        let arena = self.run.callbacks.arena();
        let Some((content_offset, placed)) = placed else {
            return;
        };
        let used = self.cells_laid_out_here(sized_child);
        // NB: A reused child tells its parent that margins do not collapse through it, which holds only for a child
        //     whose border box has a block size.
        let reusable = !placed.exports_payloads
            && self.floats.borrow().is_empty()
            && self.abspos_containing_block_info_contributions_received()
                == abspos_containing_block_info_contributions_received
            && used.border_box_block_size(false) != CssPixels::default();
        if !reusable {
            return;
        }
        let entry = in_flow_block_cache::InFlowBlockEntry {
            key,
            validity,
            cells: self.metrics_of(sized_child),
            baselines,
            offset_from_flow_position: FfiCssPixelPoint {
                x: content_offset.x,
                y: content_offset.y - flow_position,
            },
            flow_position_advance: self.flow_position() - flow_position,
            outgoing_margins: self.margin_state.borrow().clone(),
            margin_box_bottom_from_flow_position: content_offset.y
                + used.content_block_size()
                + used.margin_box_bottom(used.uses_collapsing_borders_model())
                - flow_position,
            depends_on_percentage_block_size,
            fragment_identity: placed.fragment.identity,
        };
        let committed_identity = arena.with_current_committed_fragment(node, |fragment| fragment.identity);
        self.run
            .callbacks
            .in_flow_block_cache_store()
            .store(node.slot_index(), entry, committed_identity);
    }

    fn layout_block_level_children(
        &self,
        run: &FormattingContextRun<'pass>,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        available_space_for_children: AvailableSpace,
    ) -> ChildrenSummary {
        assert!(!self.facts(block_container.node).children_are_inline());
        debug_assert!(
            !self.facts(block_container.node).is_table_wrapper(),
            "table wrappers are laid out by layout_table_wrapper_children"
        );
        let child_input = self.child_layout_input(block_container, input, available_space_for_children);
        let saved = self
            .block_offset_of_current_block_container
            .replace(Some(CssPixels::default()));
        let fragmentainer = self
            .run
            .fragmentainer
            .clone()
            .filter(|_| self.break_mode.get() != BreakMode::NoBreaks);
        let mut fragmentation_state = fragmentainer.as_deref().map(|fragmentainer| {
            let incoming = if block_container.node == self.run.box_ {
                fragmentainer.incoming.clone()
            } else {
                self.incoming_container_token.take()
            };
            FragmentationState::new(
                incoming.as_deref(),
                containing_block_position_in_root.y,
                self.fragmentainer_has_content.get(),
                self.discard_block_start_margin_of_next_child.get(),
            )
        });
        let first_child = fragmentation_state
            .as_ref()
            .and_then(|state| state.resume.as_ref()?.resume_child())
            .unwrap_or_else(|| self.run.callbacks.first_child(block_container.node));
        if block_container.node == self.run.box_ {
            self.root_flow_children.borrow_mut().clear();
        }
        let mut summary = ChildrenSummary::default();
        for child in self
            .run
            .callbacks
            .siblings_from(first_child, Node::INVALID)
            .filter(|&child| self.facts(child).is_box())
        {
            if let (Some(fragmentainer), Some(state)) = (fragmentainer.as_deref(), fragmentation_state.as_mut()) {
                match self.fragmentation_step_before_child(fragmentainer, state, block_container.node, child) {
                    FragmentationStep::Stop => break,
                    FragmentationStep::LayOut => {}
                }
            }
            let invisible = self.line_clamp_reached() && !self.facts(child).is_absolutely_positioned();
            if invisible {
                self.laying_out_invisible_line_clamp_content.set(true);
            }
            let outcome = self.layout_block_level_box(
                run,
                child,
                block_container,
                child_input,
                containing_block_position_in_root,
                None,
            );
            if invisible {
                self.laying_out_invisible_line_clamp_content.set(false);
            }
            self.fold_in_child(&mut summary, block_container.node, child, outcome.as_ref());
            if let (Some(fragmentainer), Some(state)) = (fragmentainer.as_deref(), fragmentation_state.as_mut())
                && self.fragmentation_step_after_child(
                    fragmentainer,
                    state,
                    block_container.node,
                    child,
                    outcome.as_ref(),
                ) == FragmentationStep::Stop
            {
                break;
            }
        }
        self.block_offset_of_current_block_container.set(saved);
        summary.fragmentainer_stop_token = fragmentation_state.and_then(|state| state.container_stop_token);
        summary
    }

    fn fragmentation_step_before_child(
        &self,
        fragmentainer: &fragmentation::FragmentainerContext,
        state: &mut FragmentationState,
        block_container: Node,
        child: Node,
    ) -> FragmentationStep {
        match &state.resume {
            Some(fragmentation::BlockResume::BeforeChild {
                child: resume_child, ..
            }) => {
                debug_assert_eq!(*resume_child, child);
                state.resume = None;
            }
            Some(fragmentation::BlockResume::AfterChildren | fragmentation::BlockResume::AtLine(_)) => {
                return FragmentationStep::Stop;
            }
            Some(fragmentation::BlockResume::InsideChild(token)) => {
                debug_assert_eq!(token.node, child);
                self.pending_continuation_token.replace(Some(token.clone()));
                state.resume = None;
                return FragmentationStep::LayOut;
            }
            None => {}
        }
        if !self.is_first_or_last_in_flow_candidate(child) {
            return FragmentationStep::LayOut;
        }
        if block_container == self.run.box_ && self.is_column_spanner(child) {
            return FragmentationStep::Stop;
        }
        if fragmentainer.early_break_before == Some(child) {
            if self.break_between_is_avoided(state, child) {
                if let Some(node) = state.last_child_allowing_break_before
                    && fragmentainer.request_early_break_before(node)
                {
                    return FragmentationStep::Stop;
                }
                fragmentainer.outcome_mut().violates_break_avoidance = true;
            }
            self.break_before_child(fragmentainer, state, block_container, child, false);
            return FragmentationStep::Stop;
        }
        let forced = fragmentainer.honors_forced_breaks
            && (state.previous_in_flow_child_forces_break_after
                || (fragmentation::is_forced_column_break(self.propagated_break_before(child))
                    && self.fragmentainer_has_content.get()));
        if forced {
            self.break_before_child(fragmentainer, state, block_container, child, true);
            return FragmentationStep::Stop;
        }
        if state.truncate_next_block_start_margin {
            self.margin_state.borrow_mut().reset();
            self.discard_block_start_margin_of_next_child.set(true);
        }
        state.fragmentainer_had_content_before_child = self.fragmentainer_has_content.get();
        self.break_before_child_is_avoided
            .set(self.break_between_is_avoided(state, child).then_some(child));
        FragmentationStep::LayOut
    }

    fn fragmentation_step_after_child(
        &self,
        fragmentainer: &fragmentation::FragmentainerContext,
        state: &mut FragmentationState,
        block_container: Node,
        child: Node,
        outcome: Option<&BlockLevelBoxOutcome>,
    ) -> FragmentationStep {
        if !self.is_first_or_last_in_flow_candidate(child) {
            return FragmentationStep::LayOut;
        }
        self.discard_block_start_margin_of_next_child.set(false);
        state.truncate_next_block_start_margin &= outcome.is_some_and(|outcome| outcome.collapses_through);
        let Some(outcome) = outcome else {
            return FragmentationStep::LayOut;
        };
        if let Some(broken) = &outcome.broken_inside {
            if self.allows_unforced_breaks() {
                let content_block_end = state.container_content_box_block_start_in_root
                    + self.block_offset_adjustment_from_pending_ancestor_block_start_margins(block_container)
                    + outcome.content_offset.y
                    + broken.block_end_of_content;
                if fragmentainer.exceeds(content_block_end) {
                    fragmentainer.note_space_shortage(content_block_end);
                    if state.fragmentainer_had_content_before_child
                        && fragmentation::avoids_column_break_inside(self.style(child).break_inside())
                        && fragmentainer.request_early_break_before(child)
                    {
                        return FragmentationStep::Stop;
                    }
                    fragmentainer.outcome_mut().overflows = true;
                }
            }
            self.fragmentainer_has_content.set(true);
            state.container_stop_token = self.stop_fragmentainer(
                fragmentainer,
                block_container,
                fragmentation::BlockResume::InsideChild(broken.token.clone()),
            );
            return FragmentationStep::Stop;
        }
        let break_before_is_avoided = self.break_between_is_avoided(state, child);
        let border_box_block_size = outcome.border_box_block_size();
        if self.allows_unforced_breaks() {
            let content_block_end = state.container_content_box_block_start_in_root
                + self.block_offset_adjustment_from_pending_ancestor_block_start_margins(block_container)
                + outcome.content_offset.y
                + outcome.content_block_size();
            let is_breakable = outcome.can_break_inside
                && !fragmentation::avoids_column_break_inside(self.style(child).break_inside());
            if !is_breakable {
                fragmentainer.note_unbreakable_block_size(border_box_block_size);
            }
            let block_end = content_block_end + outcome.border_box_bottom();
            if fragmentainer.exceeds(block_end) {
                fragmentainer.note_space_shortage(block_end);
                if state.fragmentainer_had_content_before_child
                    && (!is_breakable || fragmentainer.exceeds(content_block_end))
                {
                    let earlier_break = state
                        .last_child_allowing_break_before
                        .filter(|_| break_before_is_avoided);
                    if fragmentainer.request_early_break_before(earlier_break.unwrap_or(child)) {
                        return FragmentationStep::Stop;
                    }
                }
                fragmentainer.outcome_mut().overflows = true;
            }
        }
        let break_after = self.propagated_break_after(child);
        if fragmentation::is_forced_column_break(break_after) {
            state.previous_in_flow_child_forces_break_after = true;
        }
        if border_box_block_size > CssPixels::default() {
            self.fragmentainer_has_content.set(true);
            if state.fragmentainer_had_content_before_child && !break_before_is_avoided {
                state.last_child_allowing_break_before = Some(child);
            }
            state.previous_in_flow_child_avoids_break_after =
                Some(fragmentation::avoids_column_break_between(break_after));
        }
        FragmentationStep::LayOut
    }

    fn break_between_is_avoided(&self, state: &FragmentationState, child: Node) -> bool {
        state
            .previous_in_flow_child_avoids_break_after
            .is_some_and(|avoids_break_after| {
                avoids_break_after || fragmentation::avoids_column_break_between(self.propagated_break_before(child))
            })
    }

    // https://drafts.csswg.org/css-break-3/#break-propagation
    fn propagated_break_before(&self, node: Node) -> u8 {
        self.propagated_break_value(
            node,
            true,
            |style| style.break_before(),
            |node| {
                self.children(node)
                    .find(|&child| self.is_first_or_last_in_flow_candidate(child))
            },
        )
    }

    fn propagated_break_after(&self, node: Node) -> u8 {
        self.propagated_break_value(
            node,
            false,
            |style| style.break_after(),
            |node| {
                self.run
                    .callbacks
                    .children_rev(node)
                    .filter(|&child| self.facts(child).is_box())
                    .find(|&child| self.is_first_or_last_in_flow_candidate(child))
            },
        )
    }

    fn propagated_break_value(
        &self,
        node: Node,
        is_break_before: bool,
        value_of: impl Fn(StyleValues<'pass>) -> u8,
        child_of: impl Fn(Node) -> Option<Node>,
    ) -> u8 {
        let memoized = |node: Node| {
            self.propagated_break_values
                .borrow()
                .get(&(node, is_break_before))
                .copied()
        };
        let mut chain = SmallVec::<[(Node, u8); 8]>::new();
        let mut current = node;
        let mut propagated_from_below = loop {
            if let Some(value) = memoized(current) {
                break Some(value);
            }
            let value = value_of(self.style(current));
            chain.push((current, value));
            if fragmentation::is_forced_column_break(value) {
                break None;
            }
            let facts = self.facts(current);
            if !facts.is_block_container()
                || facts.children_are_inline()
                || formatting_context::formatting_context_type_created_by_box(facts).is_some()
            {
                break None;
            }
            let Some(child) = child_of(current) else {
                break None;
            };
            current = child;
        };
        for (node, own_value) in chain.into_iter().rev() {
            let value = match propagated_from_below {
                Some(below)
                    if !fragmentation::is_forced_column_break(own_value)
                        && (fragmentation::is_forced_column_break(below) || own_value == break_between::AUTO) =>
                {
                    below
                }
                _ => own_value,
            };
            self.propagated_break_values
                .borrow_mut()
                .insert((node, is_break_before), value);
            propagated_from_below = Some(value);
        }
        propagated_from_below.expect("the chain holds at least the node itself")
    }

    fn is_first_or_last_in_flow_candidate(&self, child: Node) -> bool {
        let facts = self.facts(child);
        if !facts.is_flow_layout_participant() || facts.is_floating() {
            return false;
        }
        !(self.run.callbacks.can_skip_is_anonymous_text_run(child)
            && self.style(child).white_space_collapse() == white_space_collapse::COLLAPSE)
    }

    fn allows_unforced_breaks(&self) -> bool {
        self.break_mode.get() == BreakMode::AllBreaks
    }

    fn size_continuation(
        &self,
        sized_child: ChildSizedHere<'_>,
        consumed_block_size: CssPixels,
        inner_size_excess: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        children: Option<&ChildrenSummary>,
    ) {
        let node = sized_child.node;
        let used = self.cells_laid_out_here(sized_child);
        if used.has_definite_block_size() {
            used.set_content_block_size((used.content_block_size() - consumed_block_size).max(CssPixels::default()));
            return;
        }
        let content_block_size = self.automatic_block_size_for_block_level_element_disregarding_marker(
            sized_child,
            used.available_inner_space_or_constraints_from(available_space),
            constraints,
            None,
            false,
            children,
        );
        let sized_for_whole_box = used.content_block_size();
        let consumed = consumed_block_size + inner_size_excess;
        let unconsumed_part_of_whole_box = (sized_for_whole_box - consumed).max(CssPixels::default());
        let block_size = sized_for_whole_box.min(content_block_size.max(unconsumed_part_of_whole_box));
        let clamped = self.sizing().clamp_block_size_to_min_max(
            sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
            block_size + consumed,
            available_space,
            constraints,
        );
        used.set_content_block_size((clamped - consumed).max(CssPixels::default()));
    }

    pub(crate) fn line_root_fragmentation(&self, line_root: Node) -> Option<fragmentation::LineRootFragmentation> {
        if !self.allows_unforced_breaks() {
            return None;
        }
        let fragmentainer = self.run.fragmentainer.as_deref()?;
        let incoming = if line_root == self.run.box_ {
            fragmentainer.incoming.clone()
        } else {
            self.incoming_container_token
                .borrow_mut()
                .take_if(|token| token.node == line_root)
        };
        let resume = match incoming.as_deref().map(|token| &token.resume) {
            Some(fragmentation::BlockResume::AtLine(position)) => Some(*position),
            Some(fragmentation::BlockResume::AfterChildren) => Some(fragmentation::LinePosition::AFTER_LAST_LINE),
            _ => None,
        };
        Some(fragmentation::LineRootFragmentation {
            block_size: fragmentainer.block_size,
            resume,
            fragmentainer_had_content: self.fragmentainer_has_content.get(),
            break_before_is_avoided: self.break_before_child_is_avoided.get() == Some(line_root),
        })
    }

    pub(crate) fn note_line_for_fragmentation(
        &self,
        line_block_size: CssPixels,
        line_block_end_in_root: CssPixels,
        is_kept: bool,
    ) {
        let Some(fragmentainer) = self.run.fragmentainer.as_deref() else {
            return;
        };
        fragmentainer.note_unbreakable_block_size(line_block_size);
        if fragmentainer.exceeds(line_block_end_in_root) {
            fragmentainer.note_space_shortage(line_block_end_in_root);
        }
        if is_kept {
            self.fragmentainer_has_content.set(true);
        }
    }

    pub(crate) fn note_violated_break_avoidance(&self) {
        if let Some(fragmentainer) = self.run.fragmentainer.as_deref() {
            fragmentainer.outcome_mut().violates_break_avoidance = true;
        }
    }

    pub(crate) fn note_unbreakable_line_root(&self, line_root: Node, lines_block_end_in_root: CssPixels) {
        if line_root != self.run.box_ {
            return;
        }
        let Some(fragmentainer) = self.run.fragmentainer.as_deref() else {
            return;
        };
        fragmentainer.note_unbreakable_block_size(lines_block_end_in_root);
        if fragmentainer.exceeds(lines_block_end_in_root) {
            fragmentainer.note_space_shortage(lines_block_end_in_root);
            fragmentainer.outcome_mut().overflows = true;
        }
    }

    pub(crate) fn note_overflowing_content(&self) {
        if let Some(fragmentainer) = self.run.fragmentainer.as_deref() {
            fragmentainer.outcome_mut().overflows = true;
        }
    }

    pub(crate) fn request_early_break_before(&self, node: Node) {
        if let Some(fragmentainer) = self.run.fragmentainer.as_deref() {
            fragmentainer.request_early_break_before(node);
        }
    }

    /// Stops the fragmentainer before the line of `line_root` that starts at `line_start`. The break token of a line
    /// root other than the root is returned, for the layout of the line root to break it with.
    #[must_use]
    pub(crate) fn break_line_root_before_line(
        &self,
        line_root: Node,
        line_start: fragmentation::LinePosition,
    ) -> Option<std::rc::Rc<fragmentation::BlockBreakToken>> {
        let fragmentainer = self.run.fragmentainer.as_deref()?;
        self.stop_fragmentainer(fragmentainer, line_root, fragmentation::BlockResume::AtLine(line_start))
    }

    fn box_can_break_inside(&self, node: Node, has_unbreakable_lines: bool) -> bool {
        let facts = self.facts(node);
        facts.is_block_container()
            && formatting_context::formatting_context_type_created_by_box(facts).is_none()
            && (!facts.children_are_inline() || !has_unbreakable_lines)
            && !facts.is_fieldset_box()
            && !facts.is_table_wrapper()
            && !facts.node_has_size_containment()
            && !facts
                .computed_values_view_if_styled()
                .is_some_and(fragmentation::effects_need_unbroken_box)
    }

    fn unbreakable_content_floor(&self, node: Node, children: Option<&ChildrenSummary>) -> CssPixels {
        let marker_floor =
            floor_list_item_automatic_block_size_by_marker_line_height(self.run.callbacks, node, CssPixels::default());
        if self.facts(node).is_editing_host() && !children.is_some_and(ChildrenSummary::has_in_flow_content_edge) {
            return marker_floor.max(self.style(node).line_height());
        }
        marker_floor
    }

    fn break_before_child(
        &self,
        fragmentainer: &fragmentation::FragmentainerContext,
        state: &mut FragmentationState,
        block_container: Node,
        child: Node,
        after_forced_break: bool,
    ) {
        if block_container != self.run.box_
            && state.previous_in_flow_child_avoids_break_after.is_none()
            && !after_forced_break
            && state.fragmentainer_had_content_before_container
            && fragmentainer.request_early_break_before(block_container)
        {
            return;
        }
        state.container_stop_token = self.stop_fragmentainer(
            fragmentainer,
            block_container,
            fragmentation::BlockResume::BeforeChild {
                child,
                after_forced_break,
            },
        );
    }

    /// Stops the fragmentainer inside `block_container`. The break token of a container other than the root is
    /// returned, for the layout of the container to break the container with.
    #[must_use]
    fn stop_fragmentainer(
        &self,
        fragmentainer: &fragmentation::FragmentainerContext,
        block_container: Node,
        resume: fragmentation::BlockResume,
    ) -> Option<std::rc::Rc<fragmentation::BlockBreakToken>> {
        let token = std::rc::Rc::new(fragmentation::BlockBreakToken {
            node: block_container,
            consumed_block_size: CssPixels::default(),
            resume,
        });
        if block_container == self.run.box_ {
            fragmentainer.outcome_mut().outgoing = Some(token);
            return None;
        }
        Some(token)
    }

    fn is_multicol_container(&self) -> bool {
        self.facts(self.run.box_).lays_out_in_columns()
    }

    fn lays_out_multicol_container(&self) -> bool {
        self.run.fragmentainer.is_none() && self.is_multicol_container()
    }

    fn layout_multicol_container_rows(&self, run: &FormattingContextRun<'pass>, input: LayoutInput) {
        let root = self.run.box_;
        let style = self.style(root);
        let content_inline_size = self.root_cells.content_inline_size;
        let geometry = fragmentation::resolve_column_geometry(style, content_inline_size);
        let definite_block_size = self
            .root_cells
            .has_definite_block_size
            .then_some(self.root_cells.content_block_size);
        let column_input = LayoutInput {
            available_space: AvailableSpace {
                inline_size: AvailableSize::definite(geometry.inline_size),
                block_size: definite_block_size.map_or(AvailableSize::Indefinite, AvailableSize::definite),
            },
            containing_block_constraints: input.containing_block_constraints,
            sizing: RootSizingDirectives {
                forced_content_inline_size: Some(geometry.inline_size),
                forced_content_block_size: definite_block_size,
                ..RootSizingDirectives::default()
            },
            participation: ParticipationInParentFormattingContext::Root,
        };
        let spanner_input = self.child_layout_input(self.root_block_container(), input, input.available_space);
        let is_right_to_left = style.direction() == direction::RTL;
        let column_origin = |index: usize, row_block_offset: CssPixels| FfiCssPixelPoint {
            x: geometry.column_inline_offset(index, content_inline_size, is_right_to_left),
            y: row_block_offset,
        };

        let mut baselines = formatting_context::DerivedBaselines::default();
        let mut note_baselines =
            |first: Option<CssPixels>, last: Option<CssPixels>, inline_alignment: Option<CssPixels>| {
                if let Some(first) = first {
                    baselines.first = Some(baselines.first.map_or(first, |baseline| baseline.min(first)));
                }
                if let Some(last) = last {
                    baselines.last = Some(baselines.last.map_or(last, |baseline| baseline.max(last)));
                }
                if let Some(inline_alignment) = inline_alignment {
                    baselines.inline_alignment = Some(
                        baselines
                            .inline_alignment
                            .map_or(inline_alignment, |baseline| baseline.max(inline_alignment)),
                    );
                }
            };
        let mut root_line_count = 0;
        let mut root_line_data_of_columns = Vec::new();
        let mut rows = Vec::new();
        let mut row_block_offset = CssPixels::default();
        let mut incoming = None;
        let mut follows_spanner = false;
        let block_size_limit = self.column_block_size_limit(input, definite_block_size);
        loop {
            let next_spanner = self.next_column_spanner(incoming.as_deref());
            let remaining_definite_block_size =
                definite_block_size.map(|block_size| (block_size - row_block_offset).max(CssPixels::default()));
            let remaining_block_size_limit =
                block_size_limit.map(|block_size| (block_size - row_block_offset).max(CssPixels::default()));
            let column_block_size = if next_spanner.is_some() {
                self.balanced_column_block_size(column_input, geometry, incoming.clone(), remaining_block_size_limit)
            } else {
                self.column_block_size(column_input, geometry, remaining_block_size_limit, incoming.clone())
            };
            let mut content_block_size = CssPixels::default();
            let mut column_index = 0;
            loop {
                let origin = column_origin(column_index, row_block_offset);
                let is_last_allowed_column = column_index + 1 >= fragmentation::MAXIMUM_COLUMN_COUNT;
                let ColumnLayout {
                    outcome,
                    consumed_block_size,
                    baselines: column_baselines,
                    root_line_data,
                    root,
                } = self.layout_column(
                    ColumnLayoutUse::ContainerContent,
                    column_input,
                    if is_last_allowed_column {
                        ColumnExtent::AllRemainingContent
                    } else {
                        ColumnExtent::BlockSize(column_block_size)
                    },
                    incoming.take(),
                    column_index,
                );
                if let (Some(fragments), Some(root)) = (self.run.fragments.as_deref(), root) {
                    fragments.adopt_fragmentainer_root(root, origin, root_line_count);
                }
                let filled_column = outcome
                    .outgoing
                    .as_deref()
                    .is_some_and(|token| !token.follows_forced_break());
                content_block_size = content_block_size.max(if filled_column {
                    column_block_size
                } else {
                    consumed_block_size
                });
                if let Some(line_data) = root_line_data {
                    root_line_count += line_data.lines.len();
                    root_line_data_of_columns.push((line_data, origin));
                }
                note_baselines(
                    column_baselines.first.map(|baseline| baseline + row_block_offset),
                    column_baselines.last.map(|baseline| baseline + row_block_offset),
                    column_baselines
                        .inline_alignment
                        .or(column_baselines.last)
                        .map(|baseline| baseline + row_block_offset),
                );
                let Some(outgoing) = outcome.outgoing else {
                    break;
                };
                incoming = Some(outgoing);
                column_index += 1;
            }
            let row_block_size = if style.column_fill() == column_fill::AUTO && next_spanner.is_none() {
                content_block_size
            } else {
                column_block_size
            };
            rows.push(fragmentation::ColumnRow {
                block_offset: row_block_offset,
                block_size: if next_spanner.is_none() {
                    remaining_definite_block_size.unwrap_or(row_block_size)
                } else {
                    row_block_size
                },
                column_count: column_index + 1,
            });
            row_block_offset += row_block_size;

            let Some(spanner) = next_spanner else {
                break;
            };
            if !follows_spanner || row_block_size != CssPixels::default() {
                self.margin_state.borrow_mut().reset();
                self.block_offset_of_current_block_container.set(Some(row_block_offset));
            }
            follows_spanner = true;
            let spanner_outcome = self.layout_block_level_box(
                run,
                spanner,
                self.root_block_container(),
                spanner_input,
                FfiCssPixelPoint::default(),
                None,
            );
            let spanner_baselines = spanner_outcome
                .as_ref()
                .map_or_else(DerivedBaselines::default, |outcome| {
                    outcome.baselines_in_containing_block
                });
            note_baselines(
                spanner_baselines.first,
                spanner_baselines.last,
                spanner_baselines.inline_alignment,
            );
            if let Some(margin_box_block_end) = spanner_outcome.and_then(|outcome| outcome.margin_box_block_end) {
                row_block_offset = row_block_offset.max(margin_box_block_end);
            }
            let Some(next) = self.sibling_boxes_from(self.next_sibling(spanner)).next() else {
                break;
            };
            incoming = Some(std::rc::Rc::new(fragmentation::BlockBreakToken {
                node: root,
                consumed_block_size: CssPixels::default(),
                resume: fragmentation::BlockResume::BeforeChild {
                    child: next,
                    after_forced_break: true,
                },
            }));
        }
        self.margin_state.borrow_mut().reset();
        if !root_line_data_of_columns.is_empty() {
            *self.root_lines.borrow_mut() = used_values::LineDataState::Finished(
                inline_content::InlineContent::concatenate_translated(&root_line_data_of_columns),
            );
        }
        let multicol_layout_data = std::sync::Arc::new(fragmentation::MulticolLayoutData { geometry, rows });
        self.run.update_fragment_payloads_of(self.run.box_, |payloads| {
            payloads.multicol_layout_data = Some(multicol_layout_data);
        });
        self.multicol_automatic_content_block_size.set(Some(row_block_offset));
        self.record_derived_baselines_of_root_box(baselines);
    }

    fn is_column_spanner(&self, node: Node) -> bool {
        is_column_spanner(&self.run.callbacks, node)
    }

    fn sibling_boxes_from(&self, first: Node) -> impl Iterator<Item = Node> + '_ {
        self.run
            .callbacks
            .siblings_from(first, Node::INVALID)
            .filter(|&sibling| self.facts(sibling).is_box())
    }

    fn next_column_spanner(&self, incoming: Option<&fragmentation::BlockBreakToken>) -> Option<Node> {
        if self.facts(self.run.box_).children_are_inline() {
            return None;
        }
        let first = incoming
            .and_then(|token| token.resume.resume_child())
            .unwrap_or_else(|| self.run.callbacks.first_child(self.run.box_));
        self.sibling_boxes_from(first)
            .find(|&child| self.is_column_spanner(child))
    }

    fn column_block_size_limit(&self, input: LayoutInput, definite_block_size: Option<CssPixels>) -> Option<CssPixels> {
        let root = self.run.box_;
        let style = self.style(root);
        let sizing = self.sizing();
        let constraints = input.containing_block_constraints;
        let own_size_space = AvailableSpace {
            block_size: constraints
                .percentage_basis_block_size
                .map_or(AvailableSize::Indefinite, AvailableSize::definite),
            ..input.available_space
        };
        let minimum_block_size = (definite_block_size.is_none()
            && !style.min_height().is_auto()
            && !style.min_height().is_intrinsic_sizing_constraint())
        .then(|| {
            sizing.calculate_inner_block_size(
                sizing_context::SizedBox::new(root, &self.root_cells),
                own_size_space,
                style.min_height(),
                constraints,
            )
        });
        definite_block_size.or_else(|| {
            (!sizing.should_treat_max_block_size_as_none(root, own_size_space.block_size, constraints)
                && !style.max_height().is_intrinsic_sizing_constraint())
            .then(|| {
                let maximum = sizing.calculate_inner_block_size(
                    sizing_context::SizedBox::new(root, &self.root_cells),
                    own_size_space,
                    style.max_height(),
                    constraints,
                );
                minimum_block_size.map_or(maximum, |minimum| maximum.max(minimum))
            })
        })
    }

    fn column_block_size(
        &self,
        column_input: LayoutInput,
        geometry: fragmentation::ColumnGeometry,
        maximum_block_size: Option<CssPixels>,
        incoming: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
    ) -> CssPixels {
        let style = self.style(self.run.box_);
        // https://drafts.csswg.org/css-multicol-1/#cf
        // auto: Fills columns sequentially.
        if style.column_fill() == column_fill::AUTO {
            return maximum_block_size.unwrap_or_else(|| {
                let (runs, _) = self.measure_runs_between_forced_breaks(column_input, incoming);
                runs.into_iter().max().unwrap_or_default()
            });
        }
        self.balanced_column_block_size(column_input, geometry, incoming, maximum_block_size)
    }

    fn balanced_column_block_size(
        &self,
        column_input: LayoutInput,
        geometry: fragmentation::ColumnGeometry,
        row_incoming: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
        maximum_block_size: Option<CssPixels>,
    ) -> CssPixels {
        let clamp = |block_size: CssPixels| maximum_block_size.map_or(block_size, |maximum| block_size.min(maximum));

        let (runs, tallest_unbreakable_block_size) =
            self.measure_runs_between_forced_breaks(column_input, row_incoming.clone());

        let mut implicit_breaks = vec![0usize; runs.len()];
        for _ in runs.len()..geometry.count {
            let tallest_run = (0..runs.len())
                .max_by_key(|&index| runs[index] / (implicit_breaks[index] + 1))
                .expect("content has at least one run");
            implicit_breaks[tallest_run] += 1;
        }
        let mut block_size = clamp(
            runs.iter()
                .zip(&implicit_breaks)
                .map(|(&run, &breaks)| fragmentation::ceil_divide(run, breaks + 1))
                .max()
                .unwrap_or_default()
                .max(tallest_unbreakable_block_size),
        );
        if runs.len() >= geometry.count {
            return block_size;
        }

        for _ in 0..MAXIMUM_BALANCING_ROUNDS {
            let mut incoming = row_incoming.clone();
            let mut column_count = 0;
            let mut violates_break_avoidance = false;
            let mut overflows = false;
            let mut minimal_space_shortage: Option<CssPixels> = None;
            let finished = loop {
                let ColumnLayout { outcome, .. } = self.layout_column(
                    ColumnLayoutUse::BalancingProbe,
                    column_input,
                    ColumnExtent::BlockSize(block_size),
                    incoming,
                    column_count,
                );
                column_count += 1;
                violates_break_avoidance |= outcome.violates_break_avoidance;
                overflows |= outcome.overflows;
                if let Some(shortage) = outcome.minimal_space_shortage {
                    minimal_space_shortage =
                        Some(minimal_space_shortage.map_or(shortage, |minimal| minimal.min(shortage)));
                }
                let finished = outcome.outgoing.is_none();
                incoming = outcome.outgoing;
                if finished || column_count > geometry.count {
                    break finished;
                }
            };
            if finished && column_count <= geometry.count && !violates_break_avoidance && !overflows {
                break;
            }
            let Some(shortage) = minimal_space_shortage else {
                break;
            };
            let next_block_size = clamp(block_size + shortage);
            if next_block_size <= block_size {
                break;
            }
            block_size = next_block_size;
        }
        block_size
    }

    fn measure_runs_between_forced_breaks(
        &self,
        column_input: LayoutInput,
        row_incoming: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
    ) -> (Vec<CssPixels>, CssPixels) {
        let mut runs = Vec::new();
        let mut tallest_unbreakable_block_size = CssPixels::default();
        let mut incoming = row_incoming;
        loop {
            let ColumnLayout {
                outcome,
                consumed_block_size,
                ..
            } = self.layout_column(
                ColumnLayoutUse::BalancingProbe,
                column_input,
                ColumnExtent::UntilForcedBreak,
                incoming,
                runs.len(),
            );
            tallest_unbreakable_block_size = tallest_unbreakable_block_size.max(outcome.tallest_unbreakable_block_size);
            runs.push(consumed_block_size);
            incoming = outcome.outgoing;
            if incoming.is_none() || runs.len() >= fragmentation::MAXIMUM_COLUMN_COUNT {
                break;
            }
        }
        (runs, tallest_unbreakable_block_size)
    }

    fn layout_column(
        &self,
        column_layout_use: ColumnLayoutUse,
        input: LayoutInput,
        extent: ColumnExtent,
        incoming: Option<std::rc::Rc<fragmentation::BlockBreakToken>>,
        column_index: usize,
    ) -> ColumnLayout {
        let (block_size, honors_forced_breaks) = match extent {
            ColumnExtent::BlockSize(block_size) => (Some(block_size), true),
            ColumnExtent::UntilForcedBreak => (None, true),
            ColumnExtent::AllRemainingContent => (None, false),
        };
        let seed = self
            .column_early_breaks
            .borrow()
            .seed(block_size, column_index, incoming.as_deref());
        let (mut early_break_before, mut allows_early_break_requests) = seed.unwrap_or((None, true));
        let mut tried_early_breaks: Vec<Node> = early_break_before.into_iter().collect();
        let mut minimal_space_shortage_of_discarded_attempts = None;
        let inline_items = self.column_inline_items.get_or_init(Default::default);
        let builds_fragments = match column_layout_use {
            ColumnLayoutUse::BalancingProbe => false,
            ColumnLayoutUse::ContainerContent => self.run.fragments.is_some(),
        };
        loop {
            let fragmentainer = std::rc::Rc::new(fragmentation::FragmentainerContext::new(
                block_size,
                honors_forced_breaks,
                incoming.clone(),
                inline_items.clone(),
                early_break_before,
                allows_early_break_requests,
            ));
            let outputs = formatting_context::run_fragmentainer(
                &self.run,
                self.root_cells,
                builds_fragments,
                input,
                fragmentainer.clone(),
            );
            if outputs.result.depends_on_percentage_block_size {
                self.run
                    .state
                    .note_descendant_depends_on_percentage_block_size(self.run.box_);
            }
            let mut outcome = fragmentainer.take_outcome();
            outcome.minimal_space_shortage = match (
                outcome.minimal_space_shortage,
                minimal_space_shortage_of_discarded_attempts,
            ) {
                (Some(left), Some(right)) => Some(left.min(right)),
                (left, right) => left.or(right),
            };
            if let Some(node) = outcome.needs_early_break_before {
                if tried_early_breaks.contains(&node) {
                    allows_early_break_requests = false;
                } else {
                    tried_early_breaks.push(node);
                    early_break_before = Some(node);
                    allows_early_break_requests = tried_early_breaks.len() < MAXIMUM_EARLY_BREAK_ATTEMPTS;
                }
                minimal_space_shortage_of_discarded_attempts = outcome.minimal_space_shortage;
                continue;
            }
            if seed.is_none() && (early_break_before.is_some() || !allows_early_break_requests) {
                self.column_early_breaks.borrow_mut().record(
                    block_size,
                    column_index,
                    incoming,
                    (early_break_before, allows_early_break_requests),
                );
            }
            // NB: Only the attempt that is kept tells the host what it found out.
            self.run.callbacks.receive_host_findings(outputs.host_findings);
            return ColumnLayout {
                outcome,
                consumed_block_size: outputs.result.automatic_content_block_size,
                baselines: outputs.result.baselines,
                root_line_data: outputs.root_outcome.line_data,
                root: outputs.root,
            };
        }
    }

    // Per https://www.w3.org/TR/CSS22/tables.html#model, caption boxes are block-level boxes laid
    // out inside the table wrapper box together with the table box itself. Captions remain tree
    // children of the table box, but they participate in the wrapper's block formatting context:
    // top captions first, then the table box, then bottom captions, each flowing like any other
    // block-level child.
    fn layout_table_wrapper_children(
        &self,
        run: &FormattingContextRun<'pass>,
        input: LayoutInput,
        available_space_for_children: AvailableSpace,
    ) -> ChildrenSummary {
        let wrapper = self.run.box_;
        let wrapper_block_container = self.root_block_container();
        // The table wrapper is invisible to percentage resolution: percentages on the table root
        // resolve against the wrapper's containing block, so the wrapper's own constraints pass
        // through to the table box unchanged.
        let table_box_input = LayoutInput {
            available_space: available_space_for_children,
            ..input
        };
        let caption_input = self.child_layout_input(wrapper_block_container, input, available_space_for_children);
        let saved = self
            .block_offset_of_current_block_container
            .replace(Some(CssPixels::default()));
        self.root_flow_children.borrow_mut().clear();
        let mut summary = ChildrenSummary::default();
        for child in table_wrapper_flow_children(self.run.callbacks, wrapper) {
            let child_input = if self.style(child).display().is_table_inside() {
                table_box_input
            } else {
                caption_input
            };
            let invisible = self.line_clamp_reached() && !self.facts(child).is_absolutely_positioned();
            if invisible {
                self.laying_out_invisible_line_clamp_content.set(true);
            }
            let outcome = self.layout_block_level_box(
                run,
                child,
                wrapper_block_container,
                child_input,
                FfiCssPixelPoint::default(),
                None,
            );
            if invisible {
                self.laying_out_invisible_line_clamp_content.set(false);
            }
            self.fold_in_child(&mut summary, wrapper, child, outcome.as_ref());
        }
        self.block_offset_of_current_block_container.set(saved);
        summary
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    fn layout_fieldset_with_rendered_legend(
        &self,
        run: &FormattingContextRun<'pass>,
        fieldset_block_container: BlockContainer,
        input: LayoutInput,
    ) -> ChildrenSummary {
        let fieldset = fieldset_block_container.node;
        let available_space = input.available_space;
        let child_input = self.child_layout_input(fieldset_block_container, input, available_space);
        let legend = self.facts(fieldset).rendered_legend();
        assert!(!legend.is_invalid());

        // Lay out the legend to determine its dimensions.
        let mut legend_outcome = {
            let saved = self
                .block_offset_of_current_block_container
                .replace(Some(CssPixels::default()));
            let legend_outcome = self.layout_block_level_box(
                run,
                legend,
                fieldset_block_container,
                child_input,
                FfiCssPixelPoint::default(),
                None,
            );
            self.block_offset_of_current_block_container.set(saved);
            legend_outcome
        };

        // The space allocated for the element's border on the block-start side is expected to be the element's
        // 'border-block-start-width' or the rendered legend's margin box size in the fieldset's block-flow direction,
        // whichever is greater.
        let legend_cells = legend_outcome
            .as_ref()
            .expect("the rendered legend is laid out in its fieldset")
            .cells;
        let effective_border = fieldset_block_container
            .cells
            .border_top
            .max(legend_cells.margin_box_block_size(false));
        let extra_top = effective_border - fieldset_block_container.cells.border_top;

        // Lay out non-legend children below the legend accommodation.
        self.margin_state.borrow_mut().reset();
        let mut summary = ChildrenSummary::default();
        let fieldset_is_root = fieldset == self.run.box_;
        if fieldset_is_root {
            self.root_flow_children.borrow_mut().clear();
        }
        let mut index_of_legend_in_root_flow_children = None;
        {
            let saved = self.block_offset_of_current_block_container.replace(Some(extra_top));
            for child in self.children(fieldset) {
                // NB: The legend keeps its place among the root's children, and its offset once it is placed.
                if child == legend
                    && fieldset_is_root
                    && let Some(legend_outcome) = legend_outcome.as_ref()
                {
                    let mut root_flow_children = self.root_flow_children.borrow_mut();
                    index_of_legend_in_root_flow_children = Some(root_flow_children.len());
                    root_flow_children.push(RootFlowChild::of(legend, legend_outcome));
                }
                if child != legend {
                    let invisible = self.line_clamp_reached() && !self.facts(child).is_absolutely_positioned();
                    if invisible {
                        self.laying_out_invisible_line_clamp_content.set(true);
                    }
                    let outcome = self.layout_block_level_box(
                        run,
                        child,
                        fieldset_block_container,
                        child_input,
                        FfiCssPixelPoint::default(),
                        None,
                    );
                    if invisible {
                        self.laying_out_invisible_line_clamp_content.set(false);
                    }
                    self.fold_in_child(&mut summary, fieldset, child, outcome.as_ref());
                }
            }
            self.block_offset_of_current_block_container.set(saved);
        }

        // The element is expected to be positioned in the block-flow direction such that its border box is centered over
        // the border on the block-start side of the fieldset element.
        // FIXME: Take writing modes into consideration.
        let legend_border_box_centering_offset = (effective_border - legend_cells.border_box_block_size(false)) / 2;
        let fieldset_border_box_block_start_in_content =
            -(fieldset_block_container.cells.border_top + fieldset_block_container.cells.padding_top);
        let legend_content_block_offset = fieldset_border_box_block_start_in_content
            + legend_border_box_centering_offset
            + legend_cells.border_box_top(false);
        if let Some(DeferredLegend {
            flow_position: legend_flow_position,
            placed_child: placed_legend,
            fragment_arrival: legend_fragment_arrival,
        }) = legend_outcome
            .as_mut()
            .and_then(|outcome| outcome.deferred_legend.take())
            .map(|deferred_legend| *deferred_legend)
        {
            formatting_context::place_child(
                &self.run,
                legend,
                placed_legend,
                FfiCssPixelPoint {
                    x: legend_flow_position.inline_offset,
                    y: legend_content_block_offset,
                },
                None,
                legend_fragment_arrival,
            );
            if let Some(index) = index_of_legend_in_root_flow_children
                && let Some(legend_outcome) = legend_outcome.as_ref()
            {
                self.root_flow_children.borrow_mut()[index].border_box_block_end = legend_content_block_offset
                    + legend_outcome.content_block_size()
                    + legend_outcome.border_box_bottom();
            }
            self.translate_floats_in_subtree(
                legend,
                FfiCssPixelPoint {
                    x: CssPixels::default(),
                    y: legend_content_block_offset - legend_flow_position.block_offset,
                },
            );
        }
        summary
    }

    pub(crate) fn run(&self, run: &FormattingContextRun<'pass>, input: LayoutInput) {
        self.lay_out_root_contents(run, input);
        let has_line_clamp_point = self.line_clamp_point_found.get();
        if has_line_clamp_point || self.clamped_content_is_scrollable_overflow {
            run.update_fragment_payloads_of(self.run.box_, |payloads| {
                payloads.has_line_clamp_point = has_line_clamp_point;
                payloads.clamped_content_is_scrollable_overflow = self.clamped_content_is_scrollable_overflow;
            });
        }
    }

    fn lay_out_root_contents(&self, run: &FormattingContextRun<'pass>, input: LayoutInput) {
        let available_space = input.available_space;
        if self.is_line_clamp_container && self.style(self.run.box_).max_lines() == 0 {
            let automatic_block_size = self.resolve_automatic_line_clamp_block_size(input);
            if input.sizing.finds_automatic_line_clamp_lines || run.fragments.is_none() {
                self.automatic_line_clamp_block_size.set(automatic_block_size);
            } else if automatic_block_size.is_some() {
                let measurement_result = formatting_context::lay_out_measurement_root_for_its_result(
                    self.run.callbacks,
                    self.run.box_,
                    self.root_cells,
                    LayoutInput {
                        sizing: RootSizingDirectives {
                            finds_automatic_line_clamp_lines: true,
                            ..input.sizing
                        },
                        participation: ParticipationInParentFormattingContext::Root,
                        ..input
                    },
                );
                self.max_lines.set(measurement_result.automatic_line_clamp_max_lines);
                if self.max_lines.get() == Some(0) {
                    self.line_clamp_point_found.set(true);
                }
            }
        }
        if self.lays_out_multicol_container() {
            self.layout_multicol_container_rows(run, input);
            if self.facts(self.run.box_).is_list_item_box() {
                self.layout_list_item_marker(
                    run,
                    self.root_block_container(),
                    input.sizing.outer_float_intrusion_before_list_item_children,
                    self.derived_baselines_of_root_box().first,
                );
            }
            return;
        }

        let root_input = input;
        let root_facts = self.facts(self.run.box_);
        if root_facts.is_fieldset_box() && !root_facts.rendered_legend().is_invalid() {
            let baselines = self
                .layout_fieldset_with_rendered_legend(run, self.root_block_container(), root_input)
                .baselines;
            self.record_derived_baselines_of_root_box(baselines);
            return;
        }
        let mut children_summary = if root_facts.is_table_wrapper() {
            self.layout_table_wrapper_children(run, root_input, available_space)
        } else if root_facts.children_are_inline() {
            self.layout_inline_children(
                run,
                self.root_block_container(),
                root_input,
                FfiCssPixelPoint::default(),
                available_space,
            )
        } else {
            self.layout_block_level_children(
                run,
                self.root_block_container(),
                root_input,
                FfiCssPixelPoint::default(),
                available_space,
            )
        };
        if let Some(lines) = children_summary.lines.take() {
            *self.root_lines.borrow_mut() = used_values::LineDataState::Building(Box::new(lines));
        }
        let baselines = children_summary.baselines;
        self.record_derived_baselines_of_root_box(baselines);

        // Fieldsets without a rendered legend skip collapsed margin assignment.
        if root_facts.is_fieldset_box() {
            return;
        }

        // The run's trailing collapsed margin hangs below the last real in-flow child, but it
        // aggregates margins of trailing collapse-through siblings laid out after that child was
        // placed. It is run output, not a property of that child: the child keeps its own placed
        // margin_bottom, and only the root's automatic block size consumes the aggregate.
        let collapsed_margin = self.margin_state.borrow().current_collapsed_margin();
        // NB: Only the automatic block size of a root with block-level children reads the trailing collapsed margin.
        if collapsed_margin != CssPixels::default() && !root_facts.children_are_inline() {
            let index_of_last_child_margins_do_not_collapse_through =
                self.root_flow_children.borrow().iter().rposition(|child| {
                    !self.facts(child.node).is_floating_or_absolutely_positioned() && !child.collapses_through
                });
            if let Some(index) = index_of_last_child_margins_do_not_collapse_through {
                self.trailing_collapsed_margin.set(Some((index, collapsed_margin)));
            }
        }

        if root_facts.is_list_item_box() && self.run.fragmentainer.is_none() {
            self.layout_list_item_marker(
                run,
                self.root_block_container(),
                input.sizing.outer_float_intrusion_before_list_item_children,
                self.derived_baselines_of_root_box().first,
            );
        }
    }

    /// Lays out `node`, a block-level box that interrupts the lines of `containing_block`, and returns whether margins
    /// collapse through it.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn layout_interrupting_block_inside_inline_context(
        &self,
        run: &FormattingContextRun<'pass>,
        node: Node,
        containing_block: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        line_builder: &mut line_builder::LineBuilder<'_, '_>,
        is_after_clamp_point: bool,
        offset_from_relatively_positioned_inline_ancestors: FfiCssPixelPoint,
    ) -> bool {
        let line_index = line_builder.line_index_for_block_level_box();
        let current_block_offset = line_builder.current_block_offset();
        let saved = self
            .block_offset_of_current_block_container
            .replace(Some(current_block_offset));
        if is_after_clamp_point {
            self.laying_out_invisible_line_clamp_content.set(true);
        }
        let break_mode_outside = self.break_mode.replace(BreakMode::NoBreaks);
        let outcome = self.layout_block_level_box(
            run,
            node,
            containing_block,
            input,
            containing_block_position_in_root,
            Some(InterruptedInlineContent {
                line: (!is_after_clamp_point).then_some(used_values::LineBoxFragmentCoordinate {
                    line_box_index: line_index,
                }),
                offset_from_relatively_positioned_inline_ancestors,
            }),
        );
        self.break_mode.set(break_mode_outside);
        if is_after_clamp_point {
            self.laying_out_invisible_line_clamp_content.set(false);
        }
        // SAFETY: The builder remains live and no reference escaped.
        let block_bottom = self
            .block_offset_of_current_block_container
            .get()
            .unwrap_or_else(|| line_builder.current_block_offset());
        self.block_offset_of_current_block_container.set(saved);
        let outcome = outcome.expect("the flow holds a block-level box that interrupts its lines");
        assert!(
            outcome.is_placed,
            "a block-level box is placed before its line takes it"
        );
        line_builder.append_block_level_box(
            node,
            line_index,
            block_bottom,
            self.margin_state.borrow().current_collapsed_margin(),
            line_box::BlockLevelBoxOnLine {
                content_offset: outcome.content_offset,
                cells: outcome.cells,
                baselines: formatting_context::baselines_of_block_level_box_on_line(
                    &self.run.callbacks,
                    node,
                    &outcome.cells,
                    outcome.content_baselines,
                    outcome.content_offset.y,
                ),
            },
        );
        outcome.collapses_through
    }

    /// Resolves the block size of a float that has no run of its own, sized at the inline size its block formatting
    /// context gave it, where that follows without laying out its contents.
    pub(crate) fn resolve_float_root_block_size_in_parent(&self, sized_float: ChildSizedHere<'_>, input: &LayoutInput) {
        let node = sized_float.node;
        let available_space = input.available_space;
        self.resolve_used_block_size_if_not_treated_as_auto(
            sized_float,
            available_space,
            input.containing_block_constraints,
        );
        let block_size_is_definite_from_aspect_ratio = self.metrics_of(sized_float).has_definite_inline_size()
            && self.facts(node).has_preferred_aspect_ratio()
            && self.sizing().box_is_sized_as_replaced_element(
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_float)),
                available_space,
                input.containing_block_constraints,
            );
        if self.facts(node).has_auto_content_box_size()
            || block_size_is_definite_from_aspect_ratio
            || self.style(node).display().is_flex_inside()
        {
            self.resolve_used_block_size_if_treated_as_auto(
                sized_float,
                available_space,
                input.containing_block_constraints,
                None,
                None,
            );
        }
    }

    /// Resolves the vertical box model and the inline size of a float, which depends on the floats placed beside it
    /// before.
    pub(crate) fn dimension_float_root_inline_size(
        &self,
        sized_float: ChildSizedHere<'_>,
        block_container: BlockContainer,
        input: &LayoutInput,
    ) {
        let available_space = input.available_space;
        let block_container_inline_size = block_container.cells.content_inline_size;
        self.resolve_vertical_box_model_metrics(sized_float, block_container_inline_size);
        let ParticipationInParentFormattingContext::Float {
            containing_block_position_in_bfc_root,
        } = input.participation
        else {
            unreachable!("a float root is dimensioned as a float");
        };
        let containing_block_rect = self.containing_block_rect(block_container, containing_block_position_in_bfc_root);
        let containing_block_rect_now = containing_block_rect.translated(
            CssPixels::default(),
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(block_container.node),
        );
        self.compute_inline_size(
            sized_float,
            available_space,
            input.containing_block_constraints,
            FfiCssPixelPoint {
                x: containing_block_rect_now.x,
                y: containing_block_rect_now.y,
            },
        );
    }

    /// Lays out and places `node`, a float, and returns what its insets resolved to, which is nothing for a float it
    /// leaves unplaced, the box a run of its own handed back where one laid it out, and the offset of its content box
    /// in its containing block where it placed the float.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn layout_floating_box(
        &self,
        run: &FormattingContextRun<'pass>,
        sized_float: ChildSizedHere<'_>,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        block_offset: CssPixels,
        line_builder: Option<&mut line_builder::LineBuilder<'_, '_>>,
        fragment_facts: run_state::FragmentFactsDecidedByParent,
    ) -> (
        abspos_engine::InFlowBoxInsets,
        Option<run_state::LaidOutChildBox>,
        Option<FfiCssPixelPoint>,
    ) {
        let laid_out_float = self.lay_out_float_ahead_of_its_position(
            run,
            sized_float,
            block_container,
            input,
            containing_block_position_in_root,
        );
        self.position_laid_out_float(
            run,
            laid_out_float,
            block_container,
            input,
            containing_block_position_in_root,
            block_offset,
            line_builder,
            fragment_facts,
        )
    }

    /// Lays out the contents of a float, which does not depend on where the float goes.
    pub(crate) fn lay_out_float_ahead_of_its_position(
        &self,
        run: &FormattingContextRun<'pass>,
        sized_float: ChildSizedHere<'_>,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
    ) -> FloatLaidOutAheadOfItsPosition {
        let node = sized_float.node;
        let available_space = input.available_space;
        assert!(self.facts(node).is_floating());
        let mut float_input = LayoutInput {
            available_space,
            containing_block_constraints: input.containing_block_constraints,
            sizing: RootSizingDirectives::default(),
            participation: ParticipationInParentFormattingContext::Float {
                containing_block_position_in_bfc_root: containing_block_position_in_root,
            },
        };
        // NB: The space is found ahead of the float's inline size, which can make its block size transfer through its
        //     aspect ratio.
        float_input.sizing.flex_self_block_size_resolution_space =
            formatting_context::flex_self_block_size_resolution_space(
                &self.sizing(),
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_float)),
                available_space,
                input.containing_block_constraints,
            );
        self.dimension_float_root_inline_size(sized_float, block_container, &float_input);
        let (float_layout, fragment_arrival, float_box) =
            self.layout_inside(run, sized_float, block_container, float_input, false);
        FloatLaidOutAheadOfItsPosition {
            node,
            placed_float: formatting_context::PlacedChild::of_laid_out_box(&float_box),
            cells: float_box.cells,
            laid_out_float_handed_back_by_its_run: float_layout.is_some().then_some(float_box),
            fragment_arrival,
        }
    }

    /// Positions and places a float laid out ahead of its position.
    #[allow(clippy::too_many_arguments)]
    pub(crate) fn position_laid_out_float(
        &self,
        run: &FormattingContextRun<'pass>,
        laid_out_float: FloatLaidOutAheadOfItsPosition,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        block_offset: CssPixels,
        mut line_builder: Option<&mut line_builder::LineBuilder<'_, '_>>,
        fragment_facts: run_state::FragmentFactsDecidedByParent,
    ) -> (
        abspos_engine::InFlowBoxInsets,
        Option<run_state::LaidOutChildBox>,
        Option<FfiCssPixelPoint>,
    ) {
        let FloatLaidOutAheadOfItsPosition {
            node,
            placed_float,
            cells: float_box,
            laid_out_float_handed_back_by_its_run,
            fragment_arrival,
        } = laid_out_float;
        let Some(side) = Self::float_side(self.style(node)) else {
            formatting_context::leave_child_unplaced(
                run,
                placed_float.with_fragment_facts(fragment_facts),
                fragment_arrival,
            );
            return (
                abspos_engine::InFlowBoxInsets::default(),
                laid_out_float_handed_back_by_its_run,
                None,
            );
        };
        let (floating_box, content_block_offset) = self.add_float_beside_lines(
            node,
            side,
            &float_box,
            block_container,
            input,
            containing_block_position_in_root,
            block_offset,
            line_builder.as_deref_mut(),
        );
        let margin_box_rect = floating_box.margin_box_rect_in_root_coordinate_space;

        let bottom_margin_edge = margin_box_rect.bottom();
        if let Some(fragmentainer) = self.run.fragmentainer.as_deref()
            && self.allows_unforced_breaks()
        {
            let bottom_margin_edge_in_column =
                bottom_margin_edge + self.block_offset_adjustment_from_pending_ancestor_block_start_margins(node);
            fragmentainer.note_unbreakable_block_size(margin_box_rect.height);
            if fragmentainer.exceeds(bottom_margin_edge_in_column) {
                fragmentainer.note_space_shortage(bottom_margin_edge_in_column);
                fragmentainer.outcome_mut().overflows = true;
            }
        }
        self.lowest_floating_descendant_bottom_margin_edge.set(Some(
            self.lowest_floating_descendant_bottom_margin_edge
                .get()
                .map_or(bottom_margin_edge, |lowest| lowest.max(bottom_margin_edge)),
        ));
        if let Some(line_builder) = line_builder {
            line_builder.recalculate_available_space();
        }
        let insets = self.compute_inset(run, node, block_container);

        let inline_offset = if side == FloatSide::Left {
            floating_box.offset_from_edge
        } else {
            floating_box.percentage_basis_inline_size.unwrap_or_default() - floating_box.offset_from_edge
        };
        let content_offset = FfiCssPixelPoint {
            x: inline_offset,
            y: content_block_offset,
        };
        formatting_context::place_child(
            &self.run,
            node,
            placed_float
                .with_in_flow_box_insets(insets)
                .with_fragment_facts(fragment_facts),
            content_offset,
            None,
            fragment_arrival,
        );
        (insets, laid_out_float_handed_back_by_its_run, Some(content_offset))
    }

    /// Adds a float laid out in `float_box` beside the lines `line_builder` builds, only to find where they break.
    pub(crate) fn add_float_for_finding_line_breaks(
        &self,
        node: Node,
        float_box: &used_values::RootInputCells,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        line_builder: &mut line_builder::LineBuilder<'_, '_>,
    ) {
        let Some(side) = Self::float_side(self.style(node)) else {
            return;
        };
        self.add_float_beside_lines(
            node,
            side,
            float_box,
            block_container,
            input,
            containing_block_position_in_root,
            CssPixels::default(),
            Some(&mut *line_builder),
        );
        line_builder.recalculate_available_space();
    }

    /// The floats and margins of this context before finding where lines break changes them.
    pub(crate) fn state_before_finding_line_breaks(&self) -> StateBeforeFindingLineBreaks {
        StateBeforeFindingLineBreaks {
            float_count: self.floating_box_count(),
            bands: self.bands.borrow().clone(),
            lowest_left_margin_edge: self.lowest_left_margin_edge.get(),
            lowest_right_margin_edge: self.lowest_right_margin_edge.get(),
            margin_state: self.margin_state.borrow().clone(),
        }
    }

    /// Takes out the floats added and restores the margins committed to find where lines break since `state` was the
    /// state of this context.
    pub(crate) fn restore_state_after_finding_line_breaks(&self, state: StateBeforeFindingLineBreaks) {
        *self.margin_state.borrow_mut() = state.margin_state;
        if self.floating_box_count() == state.float_count {
            return;
        }
        self.floats.borrow_mut().truncate(state.float_count);
        *self.bands.borrow_mut() = state.bands;
        self.lowest_left_margin_edge.set(state.lowest_left_margin_edge);
        self.lowest_right_margin_edge.set(state.lowest_right_margin_edge);
    }

    fn float_side(style: StyleValues<'_>) -> Option<FloatSide> {
        // FIXME: Honor writing-mode, direction and text-orientation.
        if matches!(style.float_(), float::LEFT | float::INLINE_START) {
            Some(FloatSide::Left)
        } else if matches!(style.float_(), float::RIGHT | float::INLINE_END) {
            Some(FloatSide::Right)
        } else {
            None
        }
    }

    /// Floats the box laid out in `float_box` to `side`, below the content `line_builder` already put on its line
    /// where the float does not fit beside it, and adds it to the floats of this context.
    #[allow(clippy::too_many_arguments)]
    fn add_float_beside_lines(
        &self,
        node: Node,
        side: FloatSide,
        float_box: &used_values::RootInputCells,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        block_offset: CssPixels,
        line_builder: Option<&mut line_builder::LineBuilder<'_, '_>>,
    ) -> (FloatingBox, CssPixels) {
        let style = self.style(node);
        let containing_block_rect = self.containing_block_rect(block_container, containing_block_position_in_root);
        let containing_block_rect_now = containing_block_rect.translated(
            CssPixels::default(),
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(block_container.node),
        );
        let mut margin_box_ceiling = if let Some(line_builder) = line_builder {
            line_builder.ceiling_for_float_to_be_inserted_here(float_box.margin_box_inline_size(false))
        } else {
            block_offset
        };
        if side == FloatSide::Left && matches!(style.clear(), clear::LEFT | clear::BOTH | clear::INLINE_START) {
            margin_box_ceiling =
                margin_box_ceiling.max(self.lowest_left_margin_edge.get() - containing_block_rect_now.y);
        }
        if side == FloatSide::Right && matches!(style.clear(), clear::RIGHT | clear::BOTH | clear::INLINE_END) {
            margin_box_ceiling =
                margin_box_ceiling.max(self.lowest_right_margin_edge.get() - containing_block_rect_now.y);
        }
        let mut ceiling_in_root = containing_block_rect_now.y + margin_box_ceiling;
        if let Some(last) = self.floats.borrow().last().copied() {
            ceiling_in_root = ceiling_in_root.max(
                last.margin_box_rect_in_root_coordinate_space.y
                    + self.block_offset_adjustment_from_pending_ancestor_block_start_margins(last.box_),
            );
        }
        let placement = self.place_float(
            side,
            float_box,
            input.available_space,
            containing_block_rect_now,
            ceiling_in_root,
        );
        let content_block_offset = placement.block_start - containing_block_rect_now.y
            + float_box.margin_top()
            + float_box.border_box_top(false);
        let mut margin_box_rect = Self::margin_box_rect(float_box)
            .translated(CssPixels::default(), content_block_offset)
            .translated(containing_block_rect.x, containing_block_rect.y);
        let mut floating_box = FloatingBox {
            box_: node,
            side,
            offset_from_edge: placement.offset_from_edge,
            left_margin_edge_to_content_edge: float_box.margin_left() + float_box.border_box_left(false),
            left_content_edge_to_right_margin_edge: float_box.content_inline_size()
                + float_box.margin_right()
                + float_box.border_box_right(false),
            bottom_margin_edge: content_block_offset
                + float_box.content_block_size()
                + float_box.margin_box_bottom(false),
            margin_box_rect_in_root_coordinate_space: margin_box_rect,
            containing_block_rect_in_root_coordinate_space: containing_block_rect,
            percentage_basis_inline_size: input.containing_block_constraints.percentage_basis_inline_size,
        };
        margin_box_rect.x = floating_box.margin_box_left_in_root_coordinate_space();
        floating_box.margin_box_rect_in_root_coordinate_space = margin_box_rect;
        self.floats.borrow_mut().push(floating_box);
        self.add_float_to_bands(floating_box, containing_block_rect);
        (floating_box, content_block_offset)
    }

    fn layout_inline_children(
        &self,
        run: &FormattingContextRun<'pass>,
        block_container: BlockContainer,
        input: LayoutInput,
        containing_block_position_in_root: FfiCssPixelPoint,
        available_space_for_children: AvailableSpace,
    ) -> ChildrenSummary {
        assert!(self.facts(block_container.node).children_are_inline());
        debug_assert!(
            block_container.cells.has_definite_inline_size,
            "lines are laid out in a block container of definite inline size"
        );
        let inline_input = self.child_layout_input(block_container, input, available_space_for_children);
        let mut context = inline_formatting_context::InlineFormattingContext::new_with_rust_parent(
            run,
            block_container,
            inline_input,
            containing_block_position_in_root,
            self.run.callbacks,
            self,
        );
        context.run()
    }

    fn compute_automatic_block_size_for_block_level_element(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        automatic_content_block_size_of_completed_run: Option<CssPixels>,
        children: Option<&ChildrenSummary>,
    ) -> CssPixels {
        let node = sized_child.node;
        let automatic_content_block_size = self.automatic_block_size_for_block_level_element_disregarding_marker(
            sized_child,
            available_space,
            constraints,
            automatic_content_block_size_of_completed_run,
            true,
            children,
        );
        floor_list_item_automatic_block_size_by_marker_line_height(
            self.run.callbacks,
            node,
            automatic_content_block_size,
        )
    }

    fn automatic_block_size_for_block_level_element_disregarding_marker(
        &self,
        sized_child: ChildSizedHere<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        automatic_content_block_size_of_completed_run: Option<CssPixels>,
        applies_editing_host_floor: bool,
        children: Option<&ChildrenSummary>,
    ) -> CssPixels {
        let node = sized_child.node;
        let facts = self.facts(node);
        let style = self.style(node);
        // https://drafts.csswg.org/css-contain-2/#containment-size
        // Giving an element size containment makes its principal box a size containment box and has the following
        // effects:
        // 1. The intrinsic sizes of the size containment box are determined as if the element had no content, following
        //    the same logic as when sizing as if empty.
        if facts.node_has_size_containment() {
            // https://drafts.csswg.org/css-sizing-4/#intrinsic-size-override
            // If an element has an explicit intrinsic inner size in an axis, then after laying out the element as
            // normal for size containment, the size of the contents in that axis are instead treated as being the
            // explicit intrinsic inner size instead of what was calculated in layout, and layout is performed again if
            // necessary.
            // FIXME: Nothing re-runs layout here. The substituted size replaces the block size the contents produced,
            //        and size containment keeps those contents from depending on it, so a second pass has nothing to
            //        change today.
            if style.contain_intrinsic_height_has_length() {
                return CssPixels::nearest_value_for(style.contain_intrinsic_height_px());
            }
            return CssPixels::default();
        }
        if facts.creates_block_formatting_context()
            || style.display().is_flex_inside()
            || style.display().is_grid_inside()
            || style.display().is_table_inside()
        {
            return formatting_context::independent_root_automatic_block_size(
                &self.sizing(),
                sizing_context::SizedBox::new(node, &self.metrics_of(sized_child)),
                available_space,
                constraints,
                automatic_content_block_size_of_completed_run,
            );
        }

        if let Some(automatic_content_block_size) = automatic_content_block_size_of_completed_run {
            return automatic_content_block_size;
        }

        // https://www.w3.org/TR/CSS22/visudet.html#normal-block
        // 10.6.3 Block-level non-replaced elements in normal flow when 'overflow' computes to 'visible'

        // The element's block size is the distance from its block-start content edge to the first applicable edge below.
        // 1. the bottom edge of the last line box, if the box establishes a inline formatting context with one or more lines
        if facts.children_are_inline()
            && let Some(last_line) = children.and_then(|children| children.last_line)
        {
            let mut block_size = last_line.physical_vertical_end();
            if last_line.has_block_level_box {
                let mut margin_state = self.margin_state.borrow_mut();
                let mut margin_bottom = margin_state.current_collapsed_margin();
                let metrics = self.metrics_of(sized_child);
                if metrics.padding_bottom() == CssPixels::default() && metrics.border_bottom() == CssPixels::default() {
                    margin_state.box_last_in_flow_child_margin_bottom_collapsed = true;
                    margin_bottom = CssPixels::default();
                }
                block_size = (block_size + margin_bottom).max(CssPixels::default());
            }
            return block_size;
        }

        // 2. the bottom edge of the bottom (possibly collapsed) margin of its last in-flow child, if the child's bottom margin does not collapse with the element's bottom margin
        // 3. the bottom border edge of the last in-flow child whose top margin doesn't collapse with the element's bottom margin
        if !facts.children_are_inline()
            && let Some(last_in_flow_child) = children.and_then(|children| children.last_in_flow_child)
        {
            let mut margin_state = self.margin_state.borrow_mut();
            let mut margin_bottom = if self.line_clamp_reached() {
                last_in_flow_child.margin_bottom
            } else {
                margin_state.current_collapsed_margin()
            };
            let used = self.cells_laid_out_here(sized_child);
            if used.padding_bottom() == CssPixels::default() && used.border_bottom() == CssPixels::default() {
                margin_state.box_last_in_flow_child_margin_bottom_collapsed = true;
                margin_bottom = CssPixels::default();
            }
            return (last_in_flow_child.border_box_block_end + margin_bottom).max(CssPixels::default());
        }

        // AD-HOC: Contenteditable elements must have a minimum block size (line-height) when empty, to remain clickable
        //         and usable for text input, even though this is not specified.
        //         See: https://github.com/w3c/editing/issues/70.
        if facts.is_editing_host() && applies_editing_host_floor {
            return style.line_height();
        }
        // 4. zero, otherwise
        CssPixels::default()
    }

    pub(crate) fn table_wrapper_content_inline_size(&self) -> Option<CssPixels> {
        self.table_box_in_wrapper_border_box_inline_size.get()
    }

    fn resolve_automatic_line_clamp_block_size(&self, input: LayoutInput) -> Option<CssPixels> {
        if self.root_cells.has_definite_block_size {
            return Some(self.root_cells.content_block_size);
        }

        let style = self.style(self.run.box_);
        let sizing = self.sizing();
        if style.max_height().is_auto()
            || sizing.should_treat_max_block_size_as_none(
                self.run.box_,
                input.available_space.block_size,
                input.containing_block_constraints,
            )
        {
            return None;
        }
        let mut block_size = sizing.calculate_inner_block_size(
            sizing_context::SizedBox::of_own_metrics(self.run.box_, &self.root_cells, self.run.callbacks.frame),
            input.available_space,
            style.max_height(),
            input.containing_block_constraints,
        );
        if !crate::layout::sizing_context::is_plain_auto(style.min_height()) {
            block_size = block_size.max(sizing.calculate_inner_block_size(
                sizing_context::SizedBox::of_own_metrics(self.run.box_, &self.root_cells, self.run.callbacks.frame),
                input.available_space,
                style.min_height(),
                input.containing_block_constraints,
            ));
        }
        Some(block_size)
    }

    pub(crate) fn automatic_line_clamp_max_lines(&self) -> Option<usize> {
        // https://drafts.csswg.org/css-overflow-4/#line-clamp-containers
        // The auto clamp point will be set to the last possible clamp point such that, for it and all previous
        // possible clamp points, the line-clamp container's automatic block size is not greater than the block size
        // the box would have if its automatic block size were infinite.
        let automatic_block_size = self.automatic_line_clamp_block_size.get()?;
        if self.automatic_content_block_size() <= automatic_block_size {
            return None;
        }
        self.automatic_line_clamp_max_lines
            .get()
            .or_else(|| (self.line_clamp_line_count.get() == 0).then_some(0))
    }

    pub(crate) fn automatic_content_block_size(&self) -> CssPixels {
        if let Some(block_size) = self.multicol_automatic_content_block_size.get() {
            return block_size;
        }
        let line_clamp_reached = self.line_clamp_reached();
        let block_size = automatic_block_size_for_bfc_root(
            self.root_lines.borrow().last_line(),
            self.run.callbacks,
            self.run.box_,
            &self.root_flow_children.borrow(),
            self.run.fragmentainer.is_none(),
            (!line_clamp_reached)
                .then_some(self.lowest_floating_descendant_bottom_margin_edge.get())
                .flatten(),
            (!line_clamp_reached)
                .then_some(self.trailing_collapsed_margin.get())
                .flatten(),
        );
        if self.run.fragmentainer.is_some() {
            return block_size;
        }
        floor_list_item_automatic_block_size_by_marker_line_height(self.run.callbacks, self.run.box_, block_size)
    }

    fn line_clamp_subtree_contains_line(&self, node: Node) -> bool {
        let facts = self.facts(node);
        if facts.is_text_node() {
            return !self
                .run
                .callbacks
                .text_content(node)
                .untransformed_text_is_ascii_whitespace;
        }
        if facts.is_atomic_inline() || (facts.is_anonymous() && facts.children_are_inline()) {
            return true;
        }
        if facts.has_dom_node()
            && (facts.is_floating()
                || facts.is_absolutely_positioned()
                || self.style(node).own_style_establishes_block_formatting_context()
                || !self.style(node).display().is_flow_inside())
        {
            return false;
        }
        self.children(node)
            .any(|child| self.line_clamp_subtree_contains_line(child))
    }

    fn line_clamp_has_future_content(&self, anchor: Node) -> Option<bool> {
        let mut node = anchor;
        let mut has_intervening_in_flow_box = false;
        while node != self.run.box_ {
            let mut sibling = self.next_sibling(node);
            while !sibling.is_invalid() {
                let facts = self.facts(sibling);
                if !facts.is_absolutely_positioned() && self.floats.borrow().iter().all(|float| float.box_ != sibling) {
                    if self.line_clamp_subtree_contains_line(sibling) {
                        return Some(!has_intervening_in_flow_box);
                    }
                    if facts.is_flow_layout_participant() {
                        has_intervening_in_flow_box = true;
                    }
                }
                sibling = self.next_sibling(sibling);
            }
            node = self.run.callbacks.parent(node);
        }
        has_intervening_in_flow_box.then_some(true)
    }
}

pub(crate) fn is_line_clamp_container(callbacks: &LayoutPass<'_>, node: Node) -> bool {
    let style = StyleValues::for_node(callbacks, node);
    let computed_continue = style.continue_();
    // https://drafts.csswg.org/css-overflow-4/#continue
    // If the box is a block container, then it must establish an independent formatting context that also becomes
    // a line-clamp container.
    // https://drafts.csswg.org/css-overflow-4/#webkit-line-clamp
    // The -webkit-legacy value behaves identically to collapse, except that it only takes effect if the specified
    // value of the display property is -webkit-box or -webkit-inline-box and the value of the -webkit-box-orient
    // property is vertical.
    (computed_continue == continue_value::COLLAPSE
        || (computed_continue == continue_value::_WEBKIT_LEGACY
            && style.display_before_box_type_transformation().is_webkit_box_inside()
            && style.webkit_box_orient() == webkit_box_orient::VERTICAL))
        && !NodeFacts::new(callbacks, node).lays_out_in_columns()
}

pub(crate) fn is_column_spanner(callbacks: &LayoutPass<'_>, node: Node) -> bool {
    let facts = NodeFacts::new(callbacks, node);
    facts.is_flow_layout_participant()
        && !facts.is_floating()
        && facts.display().is_block_outside()
        && StyleValues::for_node(callbacks, node).column_span() == column_span::ALL
}

pub(crate) fn box_avoids_floats_because_it_establishes_formatting_context(
    callbacks: &LayoutPass<'_>,
    node: Node,
) -> bool {
    // https://drafts.csswg.org/css2/#floats
    // The border box of a table, a block-level replaced element, or an element in the normal flow that establishes
    // a new block formatting context (such as an element with 'overflow' other than 'visible') must not overlap the
    // margin box of any floats in the same block formatting context as the element itself. If necessary,
    // implementations should clear the said element by placing it below any preceding floats, but may place it
    // adjacent to such floats if there is sufficient space. They may even make the border box of said element
    // narrower than defined by section 10.3.3. CSS2 does not define when a UA may put said element next to the
    // float or by how much said element may become narrower.

    // https://drafts.csswg.org/css-flexbox/#flex-containers
    // A flex container establishes a new flex formatting context for its contents. This is the same as establishing
    // a block formatting context, except that flex layout is used instead of block layout. For example, floats do
    // not intrude into the flex container, and the flex container’s margins do not collapse with the margins of its
    // contents.

    // https://drafts.csswg.org/css-grid/#grid-containers
    // A grid container that is not a subgrid establishes an independent grid formatting context for its contents.
    // This is the same as establishing an independent block formatting context, except that grid layout is used
    // instead of block layout: floats do not intrude into the grid container, and the grid container’s margins do
    // not collapse with the margins of its contents.
    let facts = NodeFacts::new(callbacks, node);
    facts.is_replaced_box()
        || matches!(
            formatting_context::formatting_context_type_created_by_box(facts),
            Some(
                formatting_context::FormattingContextType::Block
                    | formatting_context::FormattingContextType::Flex
                    | formatting_context::FormattingContextType::Grid
                    | formatting_context::FormattingContextType::Math
            )
        )
}

fn table_box_of_wrapper(callbacks: LayoutPass<'_>, wrapper: Node) -> Node {
    for child in callbacks.children(wrapper) {
        if NodeFacts::new(&callbacks, child).is_box()
            && StyleValues::for_node(&callbacks, child).display().is_table_inside()
        {
            return child;
        }
    }
    unreachable!("a table wrapper contains its table box")
}

// Flow-ordered children of a table wrapper's block formatting context: top captions in document
// order, then the table box, then bottom captions in document order. Captions are tree children
// of the table box; absolutely positioned captions are out of flow and are registered by
// register_table_abspos_descendants instead.
pub(crate) fn table_wrapper_flow_children(callbacks: LayoutPass<'_>, wrapper: Node) -> Vec<Node> {
    let table_box = table_box_of_wrapper(callbacks, wrapper);
    let mut flow_children = Vec::new();
    let mut bottom_captions = Vec::new();
    for child in callbacks.children(table_box) {
        let facts = NodeFacts::new(&callbacks, child);
        if facts.is_box() && facts.is_table_caption() && !facts.is_absolutely_positioned() {
            if StyleValues::for_node(&callbacks, child).caption_side() == caption_side::TOP {
                flow_children.push(child);
            } else {
                bottom_captions.push(child);
            }
        }
    }
    flow_children.push(table_box);
    flow_children.extend(bottom_captions);
    flow_children
}

/// Whether margins can collapse through `node` as far as the box itself goes, before its children are considered. The
/// block size of its border box is read only where nothing else keeps margins from collapsing through it.
pub(crate) fn margins_can_collapse_through_box_itself(
    facts: NodeFacts<'_>,
    style: StyleValues<'_>,
    border_box_block_size: impl FnOnce() -> CssPixels,
) -> bool {
    // https://drafts.csswg.org/css2/#adjoining-margins
    // Two margins are adjoining if and only if:
    // - both belong to in-flow block-level boxes that participate in the same block formatting context
    //   NB: Yes, we're dealing with one and the same box here.

    // - no line boxes, no clearance, no padding and no border separate them (Note that certain zero-height line boxes
    //   (see 9.4.2) are ignored for this purpose.)
    // NB: Border and padding are handled further down.
    if style.clear() != clear::NONE {
        return false;
    }
    // - both belong to vertically-adjacent box edges, i.e. form one of the following pairs:
    //   - top and bottom margins of a box that does not establish a new block formatting context and that has zero
    //     computed 'min-height', zero or 'auto' computed 'height', and no in-flow children
    if facts.creates_block_formatting_context() {
        return false;
    }
    // https://drafts.csswg.org/css-display-3/#independent-formatting-context
    // NOTE: [..] margins do not collapse across formatting context boundaries.
    if formatting_context::formatting_context_type_created_by_box(facts).is_some() {
        return false;
    }
    // NB: This should take care of the height and min-height constraints.
    //     ( also see https://github.com/w3c/csswg-drafts/pull/13699#issuecomment-4103045370 for spec ambiguity )
    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-margin-collapse
    // FIXME: For the purpose of margin collapsing (CSS 2 §8.3.1 Collapsing margins), if the block axis is the
    //        ratio-dependent axis, it is not considered to have a computed block-size of auto.
    border_box_block_size() == CssPixels::default()
}

// https://www.w3.org/TR/CSS22/visudet.html#root-height
pub(crate) fn automatic_block_size_for_bfc_root(
    root_last_line: Option<inline_content::LineRecord>,
    callbacks: LayoutPass<'_>,
    root: Node,
    flow_children: &[RootFlowChild],
    honors_size_containment: bool,
    lowest_floating_descendant_bottom_margin_edge: Option<CssPixels>,
    trailing_collapsed_margin: Option<(usize, CssPixels)>,
) -> CssPixels {
    let facts = NodeFacts::new(&callbacks, root);
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // A size-contained box is sized as if it had no contents.
    if honors_size_containment && facts.node_has_size_containment() {
        return CssPixels::default();
    }
    let mut bottom = None;
    if facts.children_are_inline() {
        // If it only has inline-level children, the block size is the distance between
        // the top content edge and the bottom of the bottommost line box.
        if let Some(last_line) = root_last_line {
            bottom = Some(last_line.physical_vertical_end());
            // A trailing interrupting block's bottom margin cannot collapse out of a BFC root,
            // so it contributes to the root's automatic block size. The line box bottom excludes it.
            if last_line.has_block_level_box {
                bottom = Some((bottom.unwrap() + last_line.block_level_box_block_end_margin).max(CssPixels::default()));
            }
        }
    } else {
        // If it has block-level children, the block size is the distance between
        // the top margin-edge of the topmost block-level child box
        // and the bottom margin-edge of the bottommost block-level child box.
        // NOTE: The top margin edge of the topmost block-level child box is the same as the top content edge of the root box.
        // NB: Captions flow in a table wrapper's formatting context while remaining tree children of the table box, so
        //     the flow children of a wrapper are its captions and its table box.
        let participates = |child: &RootFlowChild| {
            let child_facts = NodeFacts::new(&callbacks, child.node);
            child_facts.is_flow_layout_participant() && !child_facts.is_floating()
        };
        let last_participant = flow_children.iter().rposition(participates);
        for (index, child) in flow_children.iter().enumerate() {
            if !participates(child) || child.is_invisible_for_line_clamp {
                continue;
            }
            // Margins cannot collapse out of a BFC root: below the last real in-flow
            // child, the run's trailing collapsed margin (which folds in any trailing
            // collapse-through siblings) replaces that child's own bottom margin.
            let margin_bottom = match trailing_collapsed_margin {
                Some((index_of_last_real_child, aggregate)) if index_of_last_real_child == index => aggregate,
                // NB: The margins of an empty box collapse through it into those of the boxes after it, which
                //     are placed past them, so its own bottom margin reaches no further than theirs.
                _ if Some(index) != last_participant
                    && child.border_box_block_size == CssPixels::default()
                    && formatting_context::formatting_context_type_created_by_box(NodeFacts::new(
                        &callbacks, child.node,
                    ))
                    .is_none() =>
                {
                    CssPixels::default()
                }
                _ => child.margin_bottom,
            };
            let child_bottom = child.border_box_block_end + margin_bottom;
            bottom = Some(bottom.map_or(child_bottom, |value: CssPixels| value.max(child_bottom)));
        }
    }
    // In addition, if the element has any floating descendants
    // whose bottom margin edge is below the element's bottom content edge,
    // then the block size is increased to include those edges.
    if let Some(lowest) = lowest_floating_descendant_bottom_margin_edge {
        bottom = Some(bottom.map_or(lowest, |value| value.max(lowest)));
    }
    bottom.unwrap_or_default().max(CssPixels::default())
}

/// The automatic block size a block formatting context gives a box with a formatting context of its own ahead of the
/// box's run, while none of the box's contents are laid out.
pub(crate) fn automatic_block_size_of_independent_root_ahead_of_its_contents(
    sizing: &sizing_context::SizingContext<'_, '_>,
    sized: sizing_context::SizedBox<'_>,
    available_space: AvailableSpace,
    constraints: ContainingBlockConstraints,
) -> CssPixels {
    let node = sized.node;
    let callbacks = *sizing.callbacks();
    let facts = NodeFacts::new(&callbacks, node);
    let style = StyleValues::for_node(&callbacks, node);
    let display = style.display();
    let automatic_content_block_size = if facts.node_has_size_containment() {
        if style.contain_intrinsic_height_has_length() {
            CssPixels::nearest_value_for(style.contain_intrinsic_height_px())
        } else {
            CssPixels::default()
        }
    } else if facts.creates_block_formatting_context()
        || display.is_flex_inside()
        || display.is_grid_inside()
        || display.is_table_inside()
    {
        formatting_context::independent_root_automatic_block_size(sizing, sized, available_space, constraints, None)
    } else if facts.is_editing_host() {
        style.line_height()
    } else {
        CssPixels::default()
    };
    floor_list_item_automatic_block_size_by_marker_line_height(callbacks, node, automatic_content_block_size)
}

pub(crate) fn floor_list_item_automatic_block_size_by_marker_line_height(
    callbacks: LayoutPass<'_>,
    node: Node,
    automatic_content_block_size: CssPixels,
) -> CssPixels {
    let facts = NodeFacts::new(&callbacks, node);
    if !facts.is_list_item_box() {
        return automatic_content_block_size;
    }
    let marker = facts.list_item_marker();
    if marker.is_invalid() {
        return automatic_content_block_size;
    }
    automatic_content_block_size.max(StyleValues::for_node(&callbacks, marker).line_height())
}

/// The inline layout of the table box inside `child` that `kept` holds for the run of `child`, which stays kept for
/// another child.
pub(crate) fn take_table_inline_layout_kept_for(
    kept: &Cell<Option<(Node, table_formatting_context::TableInlineLayout)>>,
    child: Node,
) -> Option<table_formatting_context::TableInlineLayout> {
    let (kept_for, layout) = kept.take()?;
    if kept_for != child {
        kept.set(Some((kept_for, layout)));
        return None;
    }
    Some(layout)
}
