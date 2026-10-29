/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(crate) struct AtomicRootInlineSizeResolution {
    pub(crate) content_inline_size: CssPixels,
    pub(crate) width_was_treated_as_auto: bool,
    pub(crate) max_content_size_that_fit_the_definite_available_inner_space: Option<CssPixels>,
}

/// A block size a sizing step resolves for a box, and whether it makes the box's block size definite.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ResolvedBlockSize {
    pub(crate) content_block_size: CssPixels,
    pub(crate) makes_block_size_definite: bool,
}

impl ResolvedBlockSize {
    pub(crate) fn apply_to_cells(self, cells: &mut used_values::RootInputCells) {
        cells.set_content_block_size(self.content_block_size);
        if self.makes_block_size_definite {
            cells.has_definite_block_size = true;
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct OuterInlineContributions {
    pub(crate) min_content_inline_size: CssPixels,
    pub(crate) max_content_inline_size: CssPixels,
    pub(crate) margin_start: CssPixels,
    pub(crate) border_start: CssPixels,
    pub(crate) padding_start: CssPixels,
    pub(crate) padding_end: CssPixels,
    pub(crate) border_end: CssPixels,
    pub(crate) margin_end: CssPixels,
}

impl OuterInlineContributions {
    pub(crate) fn margin_box_contributions(&self) -> ContentSizes {
        let margin_box_inline_size = |content_inline_size| {
            line_box::inline_advance(
                self.margin_start,
                self.border_start + self.padding_start,
                content_inline_size,
                self.padding_end + self.border_end,
                self.margin_end,
            )
        };
        ContentSizes {
            min: margin_box_inline_size(self.min_content_inline_size),
            max: margin_box_inline_size(self.max_content_inline_size),
        }
    }
}

/// Inline size limits transferred from the block axis through a preferred aspect ratio.
#[derive(Clone, Copy)]
pub(crate) struct TransferredInlineSizeLimits {
    pub(crate) minimum: CssPixels,
    pub(crate) maximum: Option<CssPixels>,
}

impl TransferredInlineSizeLimits {
    pub(crate) fn clamp(&self, inline_size: CssPixels) -> CssPixels {
        self.maximum
            .map_or(inline_size, |maximum| inline_size.min(maximum))
            .max(self.minimum)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ContentSizes {
    pub(crate) min: CssPixels,
    pub(crate) max: CssPixels,
}

impl ContentSizes {
    pub(crate) fn largest_of(self, other: Self) -> Self {
        Self {
            min: self.min.max(other.min),
            max: self.max.max(other.max),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ContentInlineSizes {
    pub(crate) sizes: ContentSizes,
    pub(crate) first_line_max_content: FirstLineMaxContent,
    pub(crate) dependencies: IntrinsicMeasurementDependencies,
}

#[derive(Clone, Copy)]
struct ContentInlineSizeAnswer {
    size: CssPixels,
    first_line_max_content: Option<FirstLineMaxContent>,
    dependencies: IntrinsicMeasurementDependencies,
}

impl ContentInlineSizeAnswer {
    fn from_cache(cached: IntrinsicInlineSizeMeasurement, kind: IntrinsicSizeCacheKind) -> Self {
        let (size, first_line_max_content) = match kind {
            IntrinsicSizeCacheKind::MinContentInline => (cached.min_content_inline_size, None),
            IntrinsicSizeCacheKind::MaxContentInline => {
                (cached.max_content_inline_size, Some(cached.first_line_max_content))
            }
            _ => unreachable!("inline measurement cache kind must use the inline axis"),
        };
        Self {
            size,
            first_line_max_content,
            dependencies: cached.dependencies(),
        }
    }
}

#[derive(Clone, Copy)]
pub(crate) struct TableWrapperBlockSizes {
    pub(crate) table_box_border_box_block_size: CssPixels,
    /// The table box plus its captions.
    pub(crate) wrapper_content_block_size: CssPixels,
}

/// A box a sizing step sizes, with the sizes, size constraints and box sides the step reads of it: held in cells of its
/// own or in cells of the parent that sizes it.
#[derive(Clone, Copy)]
pub(crate) struct SizedBox<'metrics> {
    pub(crate) node: Node,
    /// What the caller holds of the box, which is nothing for a box its run does not hold yet.
    metrics: Option<&'metrics dyn BoxMetricCells>,
    /// Where the metrics are the box's own (held by the run or call that sizes the box, not by a parent sizing it ahead
    /// of its run), the frame they are held in. content_block_size_of_orthogonal_flow() reads such metrics in the box's
    /// own frame.
    own_metrics_frame: Option<geometry::Frame>,
}

impl<'metrics> SizedBox<'metrics> {
    pub(crate) fn new(node: Node, metrics: &'metrics dyn BoxMetricCells) -> Self {
        Self {
            node,
            metrics: Some(metrics),
            own_metrics_frame: None,
        }
    }

    /// `node`, with metrics of its own that are held in frame `held_in`.
    pub(crate) fn of_own_metrics(node: Node, metrics: &'metrics dyn BoxMetricCells, held_in: geometry::Frame) -> Self {
        Self {
            node,
            metrics: Some(metrics),
            own_metrics_frame: Some(held_in),
        }
    }

    /// `node`, of which its caller holds no metrics.
    pub(crate) fn unheld(node: Node) -> Self {
        Self {
            node,
            metrics: None,
            own_metrics_frame: None,
        }
    }

    /// The metrics of the box, which a step reads only of a box its caller holds.
    #[track_caller]
    pub(crate) fn metrics(&self) -> &'metrics dyn BoxMetricCells {
        match self.metrics {
            Some(metrics) => metrics,
            None => panic!("{:?} is sized without metrics its caller holds", self.node),
        }
    }

    pub(crate) fn metrics_if_held(&self) -> Option<&'metrics dyn BoxMetricCells> {
        self.metrics
    }
}

/// A box a sizing step sizes in its own cells, held in `frame`. The step reads the cells in that frame and writes to
/// them what it resolves.
#[derive(Clone, Copy)]
pub(crate) struct SizedCells<'cells> {
    pub(crate) node: Node,
    pub(crate) cells: &'cells Cell<used_values::RootInputCells>,
    pub(crate) frame: geometry::Frame,
}

impl<'cells> SizedCells<'cells> {
    pub(crate) fn new(node: Node, cells: &'cells Cell<used_values::RootInputCells>, frame: geometry::Frame) -> Self {
        Self { node, cells, frame }
    }

    pub(crate) fn sized_box(&self) -> SizedBox<'cells> {
        SizedBox::of_own_metrics(self.node, self.cells, self.frame)
    }
}

pub(crate) struct SizingContext<'pass, 'container> {
    state: &'pass RunState,
    callbacks: LayoutPass<'pass>,
    style_cache: Cell<Option<(Node, &'pass FfiStylePayloads)>>,
    child_run_shapes: Option<&'container dyn formatting_context::ChildRunShapes>,
}

/// Whether a size is `auto` itself, rather than a calc-size() with an auto basis.
pub(crate) fn is_plain_auto(size: &ComputedSize) -> bool {
    size.is_auto() && size.calc_size().is_none()
}

/// The block axis margins that stretch-fit sizing subtracts from the containing block size, with percentages of
/// `margin_basis`.
/// https://drafts.csswg.org/css-sizing-3/#stretch-fit-sizing
pub(crate) fn stretch_fit_block_margins(
    callbacks: &LayoutPass<'_>,
    node: Node,
    margin_basis: CssPixels,
) -> (CssPixels, CssPixels) {
    let style = StyleValues::for_node(callbacks, node);
    let mut margin_top = style.margin_top().to_px(margin_basis);
    let mut margin_bottom = style.margin_bottom().to_px(margin_basis);
    // For in-flow block-level boxes when resolving the block axis size: if the ancestor element percentages resolve
    // against does not have a block-start border or padding and is not an independent formatting context, treat the
    // element's block-start margin as zero for the purpose of calculating this size. Do the same for the block-end
    // margin, analogously.
    let facts = NodeFacts::new(callbacks, node);
    if !(facts.is_in_flow() && !facts.is_inline() && facts.parent_is_unfloated_flow_container()) {
        return (margin_top, margin_bottom);
    }
    let containing_block = callbacks.in_flow_containing_block(node);
    if containing_block.is_invalid()
        || formatting_context::formatting_context_type_created_by_box(NodeFacts::new(callbacks, containing_block))
            .is_some()
    {
        return (margin_top, margin_bottom);
    }
    // NB: The containing block's used values belong to another run, so its padding is resolved from its style.
    let containing_block_style = StyleValues::for_node(callbacks, containing_block);
    if containing_block_style.border_top_width() == CssPixels::default()
        && containing_block_style.padding_top().to_px(margin_basis) == CssPixels::default()
    {
        margin_top = CssPixels::default();
    }
    if containing_block_style.border_bottom_width() == CssPixels::default()
        && containing_block_style.padding_bottom().to_px(margin_basis) == CssPixels::default()
    {
        margin_bottom = CssPixels::default();
    }
    (margin_top, margin_bottom)
}

impl<'pass, 'container> SizingContext<'pass, 'container> {
    #[inline]
    pub(crate) fn new(state: &'pass RunState, callbacks: LayoutPass<'pass>) -> Self {
        Self {
            state,
            callbacks,
            style_cache: Cell::new(None),
            child_run_shapes: None,
        }
    }

    pub(crate) fn with_child_run_shapes(
        self,
        child_run_shapes: &'container dyn formatting_context::ChildRunShapes,
    ) -> Self {
        Self {
            child_run_shapes: Some(child_run_shapes),
            ..self
        }
    }

    pub(super) fn callbacks(&self) -> &LayoutPass<'pass> {
        &self.callbacks
    }

    #[inline]
    pub(super) fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    pub(super) fn style(&self, node: Node) -> StyleValues<'pass> {
        let kind = self.callbacks.node_data(node).kind.get();
        let payloads = match self.style_cache.get() {
            Some((cached_node, payloads)) if cached_node == node => payloads,
            _ => {
                let payloads = self.callbacks.style_payloads(node);
                self.style_cache.set(Some((node, payloads)));
                payloads
            }
        };
        let style = StyleValues::new(payloads).for_kind(kind).in_frame(self.callbacks.frame);
        if !style.is_absolutely_positioned() {
            return style;
        }
        style.with_anchor_resolved_values(self.state.anchor_resolved_values_of(node))
    }

    fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    /// Whether a box without children can still have an intrinsic size, as a grid container does from its tracks.
    fn has_size_without_children(&self, node: Node) -> bool {
        formatting_context::formatting_context_type_created_by_box(self.facts(node))
            == Some(formatting_context::FormattingContextType::Grid)
    }

    fn has_children(&self, node: Node) -> bool {
        !self.callbacks.first_child(node).is_invalid()
    }

    fn intrinsic_inline_measurement_may_depend_on_block_size(&self, root: Node, block_size: Option<CssPixels>) -> bool {
        if block_size.is_none() {
            return true;
        }
        // OPTIMIZATION: A definite block size only distinguishes intrinsic inline measurements when a cross-axis
        // dependency can transfer it back into the inline axis. Reuse the common horizontal-flow measurement across
        // assigned block sizes, while preserving distinct entries for orthogonal flows and aspect-ratio transfers.
        self.intrinsic_inline_size_may_depend_on_block_size(root)
    }

    pub(crate) fn intrinsic_inline_size_may_depend_on_block_size(&self, root: Node) -> bool {
        self.callbacks
            .intrinsic_size_caches()
            .intrinsic_inline_size_depends_on_block_size(self.callbacks.intrinsic_size_cache_stamp(root), || {
                let mut pending = vec![root];
                while let Some(node) = pending.pop() {
                    let facts = self.facts(node);
                    if !facts.is_text_node() {
                        let style = self.style(node);
                        if geometry::Frame::of(style.writing_mode()).is_orthogonal_to(self.callbacks.frame) {
                            return true;
                        }
                        if style.display().is_flex_inside()
                            && matches!(
                                style.effective_flex_direction(),
                                flex_direction::COLUMN | flex_direction::COLUMN_REVERSE
                            )
                            && style.flex_wrap() != flex_wrap::NOWRAP
                        {
                            return true;
                        }
                        if facts.has_preferred_aspect_ratio()
                            && (style.height().contains_percentage()
                                || style.min_height().contains_percentage()
                                || style.max_height().contains_percentage()
                                || (style.height().is_auto()
                                    && (node_facts::has_flag(facts.data(), NodeFlag::IsFlexItem)
                                        || node_facts::has_flag(facts.data(), NodeFlag::IsGridItem))))
                        {
                            return true;
                        }
                    }
                    for child in self.callbacks.children(node) {
                        pending.push(child);
                    }
                }
                false
            })
    }

    pub(crate) fn content_block_size_from_aspect_ratio(
        &self,
        sized: SizedBox<'_>,
        content_inline_size: CssPixels,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style(node);
        let metrics = sized.metrics();
        formatting_context::content_block_size_from_aspect_ratio_values(
            content_inline_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + metrics.padding_left(),
            style.border_right_width() + metrics.padding_right(),
            style.border_top_width() + metrics.padding_top(),
            style.border_bottom_width() + metrics.padding_bottom(),
        )
    }

    pub(crate) fn content_inline_size_from_aspect_ratio(
        &self,
        sized: SizedBox<'_>,
        content_block_size: CssPixels,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style(node);
        let metrics = sized.metrics();
        formatting_context::content_inline_size_from_aspect_ratio_values(
            content_block_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + metrics.padding_left(),
            style.border_right_width() + metrics.padding_right(),
            style.border_top_width() + metrics.padding_top(),
            style.border_bottom_width() + metrics.padding_bottom(),
        )
    }

    fn auto_content_size(&self, node: Node) -> formatting_context::ReplacedIntrinsicSize {
        self.facts(node).auto_content_size()
    }

    fn default_replaced_size(&self, axis: SizingAxis) -> CssPixels {
        // NB: The 300px by 150px default object size is physical, while sizing uses the current frame's axes.
        let physical_width = matches!(axis, SizingAxis::Inline) != self.callbacks.frame.is_transposed();
        CssPixels::from_integer(if physical_width { 300 } else { 150 })
    }

    fn intrinsic_size_for_replaced_sizing(&self, node: Node) -> formatting_context::ReplacedIntrinsicSize {
        let auto_size = self.auto_content_size(node);
        if auto_size.width.is_some() || auto_size.height.is_some() || auto_size.aspect_ratio.is_some() {
            return auto_size;
        }
        let facts = self.facts(node);
        // https://drafts.csswg.org/css-ui-4/#appearance-switching
        // The element is rendered following the usual rules of CSS. Replaced elements other than widgets are not affected
        // by this and remain replaced elements. Widgets must not have their native appearance, and instead must have their
        // primitive appearance.
        //
        // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget
        // An input element whose type attribute is in one of the above states is an element with default preferred size,
        // and user agents are expected to apply the 'field-sizing' CSS property to the element.
        formatting_context::ReplacedIntrinsicSize {
            width: facts.default_preferred_width(),
            height: facts.default_preferred_height(),
            aspect_ratio: None,
        }
    }

    fn max_content_size_for_replaced_element_without_natural_size(
        &self,
        sized: SizedBox<'_>,
        natural_size: formatting_context::ReplacedIntrinsicSize,
        dimension: formatting_context::SizeDimension,
        constraints: formatting_context::ReplacedMaxContentSizeLimits,
    ) -> Option<CssPixels> {
        let node = sized.node;
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        // the intrinsic sizes of replaced elements without natural sizes are defined below:
        let facts = self.facts(node);
        let is_inline_axis = dimension == formatting_context::SizeDimension::Inline;
        if !facts.is_replaced_box()
            || if is_inline_axis {
                natural_size.width.is_some()
            } else {
                natural_size.height.is_some()
            }
        {
            return None;
        }

        // SVG Integration says that a non-top-level <svg> starts with auto width/height, and that with a viewBox, missing
        // width/height attributes "keep" their auto value. The resulting width, height, and aspect ratio are then
        // "used in CSS sizing as intrinsic element size properties".
        //
        // CSS Sizing defines max-content as the size the box would have "if it was a float" with an auto preferred size.
        // CSS2 replaced sizing then resolves auto width from "(used height) * (intrinsic ratio)", and auto height from
        // "(used width) / (intrinsic ratio)". Keep this SVG specific bridge before falling through to CSS Sizing's fallback
        // for replaced elements without natural sizes.
        //  - https://svgwg.org/specs/integration/#svg-css-sizing
        //  - https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        //  - https://drafts.csswg.org/css2/#inline-replaced-width
        //  - https://drafts.csswg.org/css2/#inline-replaced-height
        if facts.is_svg_svg_box()
            && let Some(ratio) = natural_size.aspect_ratio
        {
            if is_inline_axis {
                if let Some(height) = natural_size.height {
                    return Some(ratio.multiply(height));
                }
            } else if let Some(width) = natural_size.width {
                return Some(ratio.divide(width));
            }
        }

        // For the max-content size:
        // If it has a preferred aspect ratio:
        if facts.has_preferred_aspect_ratio() {
            if let Some(size) = constraints.definite_size_in_ratio_determining_axis {
                // If the available space is definite in the inline axis, use the stretch fit into that size for the inline size
                // and calculate the block size using the aspect ratio.
                //
                // NB: This helper is only for the max-content size, which has no definite available inline size. Callers may
                //     still know a definite used size in the opposite axis when the box lacks a natural size in that axis.
                return Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(sized, size)
                } else {
                    self.content_block_size_from_aspect_ratio(sized, size)
                });
            }

            let style = self.style(node);
            // Otherwise if the box has a <length> as its computed value for min-width or min-height, use that size and
            // calculate the other dimension using the aspect ratio; if both dimensions have a <length> minimum, choose the
            // one that results in the larger overall size.
            //
            // NOTE: This case was previous calculated from a 300x150 default size, rather than the box’s min size. This is
            //       believed to be a better behavior, and likely to be Web-compatible, but please send feedback to the CSSWG
            //       if there are any problems.
            let size_from_min_inline = if let Some(inline_size) = constraints.minimum_inline_size {
                Some(if is_inline_axis {
                    inline_size
                } else {
                    self.content_block_size_from_aspect_ratio(sized, inline_size)
                })
            } else if style.min_width().is_length_percentage() && !style.min_width().contains_percentage() {
                let inline_size = style.min_width().to_px(CssPixels::default());
                Some(if is_inline_axis {
                    inline_size
                } else {
                    self.content_block_size_from_aspect_ratio(sized, inline_size)
                })
            } else {
                None
            };
            let size_from_min_block = if let Some(block_size) = constraints.minimum_block_size {
                Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(sized, block_size)
                } else {
                    block_size
                })
            } else if style.min_height().is_length_percentage() && !style.min_height().contains_percentage() {
                let block_size = style.min_height().to_px(CssPixels::default());
                Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(sized, block_size)
                } else {
                    block_size
                })
            } else {
                None
            };

            return match (size_from_min_inline, size_from_min_block) {
                (Some(inline), Some(block)) => Some(inline.max(block)),
                (Some(inline), None) => Some(inline),
                (None, Some(block)) => Some(block),
                (None, None) => Some(if is_inline_axis {
                    // Otherwise use an inline size matching the corresponding dimension of the initial containing block and calculate
                    // the other dimension using the aspect ratio.
                    //
                    // NOTE: This author-controllable behavior is made possible by the new auto value for the min size properties.
                    //       This is believed to be a better behavior, but it is not yet clear if it is Web-compatible, so please
                    //       send feedback to the CSSWG if there are any problems.
                    facts.initial_containing_block_inline_size()
                } else {
                    self.content_block_size_from_aspect_ratio(sized, facts.initial_containing_block_inline_size())
                }),
            };
        }

        // If it has no preferred aspect ratio:
        // For both the min-content size and max-content size:
        // If the box has a <length> as its computed minimum size (min-width/min-height) in that dimension, use that size.
        let min_size = if is_inline_axis {
            self.style(node).min_width()
        } else {
            self.style(node).min_height()
        };
        if min_size.is_length_percentage() && !min_size.contains_percentage() {
            return Some(min_size.to_px(CssPixels::default()));
        }
        // Otherwise, use 300px for the width and/or 150px for the height as needed.
        Some(self.default_replaced_size(if is_inline_axis {
            SizingAxis::Inline
        } else {
            SizingAxis::Block
        }))
    }

    fn tentative_inline_size_for_replaced_element(
        &self,
        sized: SizedBox<'_>,
        computed_inline_size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let inline_size = self.tentative_inline_size_for_replaced_element_before_calc_size(
            sized,
            computed_inline_size,
            available_space,
            constraints,
        );
        if !computed_inline_size.is_auto() {
            return inline_size;
        }
        self.resolve_calc_size(
            sized,
            SizingAxis::Inline,
            computed_inline_size,
            inline_size,
            constraints.percentage_basis_inline_size,
        )
    }

    fn tentative_inline_size_for_replaced_element_before_calc_size(
        &self,
        sized: SizedBox<'_>,
        computed_inline_size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // 10.3.2 Inline, replaced elements, https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
        // Treat percentages of indefinite containing block widths as 0 (the initial width).
        if computed_inline_size.is_percentage() && constraints.percentage_basis_inline_size.is_none() {
            return CssPixels::default();
        }
        let style = self.style(node);
        let preferred_block_size = self.preferred_block_size(node, constraints);
        let computed_block_size = if preferred_block_size.is_auto() {
            preferred_block_size
        } else if self.should_treat_block_size_as_auto(sized, available_space, constraints) {
            auto_computed_size()
        } else {
            preferred_block_size
        };
        // NB: A height given as calc-size() resolves to a used height that the width follows through the ratio.
        let block_size_is_auto = is_plain_auto(computed_block_size);
        let used_inline_size = if computed_inline_size.is_auto() {
            computed_inline_size.to_px(available_space.inline_size.to_px_or_zero())
        } else {
            self.calculate_inner_inline_size(sized, available_space.inline_size, computed_inline_size, constraints)
        };
        // If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic width,
        // then that intrinsic width is the used value of 'width'.
        let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
        if block_size_is_auto
            && computed_inline_size.is_auto()
            && let Some(width) = intrinsic.width
        {
            return width;
        }
        // If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width,
        // but does have an intrinsic height and intrinsic ratio;
        // or if 'width' has a computed value of 'auto',
        // 'height' has some other computed value, and the element does have an intrinsic ratio; then the used value of 'width' is:
        //
        //     (used height) * (intrinsic ratio)
        let has_ratio = self.facts(node).has_preferred_aspect_ratio();
        if (block_size_is_auto
            && computed_inline_size.is_auto()
            && intrinsic.width.is_none()
            && intrinsic.height.is_some()
            && has_ratio)
            || (computed_inline_size.is_auto() && !block_size_is_auto && has_ratio)
        {
            let block_size = self.compute_block_size_for_replaced_element(sized, available_space, constraints);
            let inline_size = self.content_inline_size_from_aspect_ratio(sized, block_size);
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
            let minimum = self.automatic_minimum_size_from_aspect_ratio(
                sized,
                SizingAxis::Inline,
                block_size,
                inline_size,
                available_space,
                constraints,
                None,
            );
            return minimum.map_or(inline_size, |minimum| inline_size.max(minimum));
        }
        // If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but no intrinsic height or width,
        // then the used value of 'width' is undefined in CSS 2.2. However, it is suggested that, if the containing block's width does not itself
        // depend on the replaced element's width, then the used value of 'width' is calculated from the constraint equation used for block-level,
        // non-replaced elements in normal flow.
        if block_size_is_auto
            && computed_inline_size.is_auto()
            && intrinsic.width.is_none()
            && intrinsic.height.is_none()
            && has_ratio
        {
            if !available_space.inline_size.is_intrinsic_sizing_constraint() {
                return self.calculate_stretch_fit_inline_size(sized, available_space.inline_size);
            }
            match formatting_context::cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                style.width().contains_percentage(),
                available_space.inline_size,
                formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                formatting_context::CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    return CssPixels::default();
                }
                formatting_context::CyclicPercentageIntrinsicContribution::TreatAsInitialValue => {}
                formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => {
                    return self.calculate_stretch_fit_inline_size(sized, available_space.inline_size);
                }
            }
        }
        // Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that intrinsic width is the used value of 'width'.
        //
        // Otherwise, if 'width' has a computed value of 'auto', but none of the conditions above are met, then the used value of 'width' becomes 300px.
        // If 300px is too wide to fit the device, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead.
        if computed_inline_size.is_auto() {
            if let Some(width) = intrinsic.width {
                return width;
            }
            return self.default_replaced_size(SizingAxis::Inline);
        }
        used_inline_size
    }

    fn tentative_block_size_for_replaced_element(
        &self,
        sized: SizedBox<'_>,
        computed_block_size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let block_size = self.tentative_block_size_for_replaced_element_before_calc_size(
            sized,
            computed_block_size,
            available_space,
            constraints,
        );
        if !computed_block_size.is_auto() {
            return block_size;
        }
        self.resolve_calc_size(
            sized,
            SizingAxis::Block,
            computed_block_size,
            block_size,
            constraints.percentage_basis_block_size,
        )
    }

    fn tentative_block_size_for_replaced_element_before_calc_size(
        &self,
        sized: SizedBox<'_>,
        computed_block_size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
        // https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-height
        let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
        // If 'height' and 'width' both have computed values of 'auto' and the element also has
        // an intrinsic height, then that intrinsic height is the used value of 'height'.
        // NB: The automatic height a calc-size() height resolves against is the intrinsic one, not one that follows
        //     the width, which itself follows the resolved height.
        // NB: A min-height or max-height can stand in for 'height' here, so a length of theirs is not taken as 'auto'
        //     although the box's own height is. An intrinsic sizing keyword of theirs is the intrinsic height.
        let block_size_takes_intrinsic_height = || match computed_block_size.calc_size() {
            Some(_) => computed_block_size.is_auto(),
            None => {
                (computed_block_size.is_auto() || computed_block_size.is_intrinsic_sizing_constraint())
                    && self.should_treat_block_size_as_auto(sized, available_space, constraints)
            }
        };
        if self.should_treat_inline_size_as_auto(sized, available_space)
            && self.style(node).width().calc_size().is_none()
            && block_size_takes_intrinsic_height()
            && let Some(height) = intrinsic.height
        {
            return height;
        }
        // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the used value of 'height' is:
        //
        //     (used width) / (intrinsic ratio)
        if computed_block_size.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            return self.content_block_size_from_aspect_ratio(sized, sized.metrics().content_inline_size());
        }
        // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then that intrinsic height is the used value of 'height'.
        //
        // Otherwise, if 'height' has a computed value of 'auto', but none of the conditions above are met,
        // then the used value of 'height' must be set to the height of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px,
        // and has a width not greater than the device width.
        if computed_block_size.is_auto() {
            return intrinsic
                .height
                .unwrap_or_else(|| self.default_replaced_size(SizingAxis::Block));
        }
        // FIXME: Handle cases when available_space is not definite.
        self.calculate_inner_block_size(sized, available_space, computed_block_size, constraints)
    }

    fn solve_replaced_size_constraint(
        &self,
        sized: SizedBox<'_>,
        input_inline_size: CssPixels,
        input_block_size: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> (CssPixels, CssPixels) {
        let node = sized.node;
        // 10.4 Minimum and maximum widths: 'min-width' and 'max-width'
        // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
        let style = self.style(node);
        // https://drafts.csswg.org/css-sizing-4/#valdef-aspect-ratio-ratio
        // The box’s preferred aspect ratio is the specified ratio of width / height.
        // NB: The natural dimensions can describe a different ratio. Transfer the tentative inline size through
        //     the specified ratio before resolving constraints on either dimension. This also applies to an auto
        //     fallback ratio when no natural ratio is available, including under size containment.
        let input_block_size = if self.facts(node).has_preferred_aspect_ratio()
            && (!style.aspect_ratio_uses_natural_when_available()
                || self.facts(node).node_has_size_containment()
                || self.auto_content_size(node).aspect_ratio.is_none())
        {
            self.content_block_size_from_aspect_ratio(sized, input_inline_size)
        } else {
            input_block_size
        };
        let min_inline = if is_plain_auto(style.min_width()) {
            CssPixels::default()
        } else {
            self.calculate_inner_inline_size(sized, available_space.inline_size, style.min_width(), constraints)
        };
        // NB: A maximum of 'none' places no limit, so the table can grow a size to satisfy a minimum in the other axis.
        let specified_max_inline =
            if self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
                max_dimension_value()
            } else {
                self.calculate_inner_inline_size(sized, available_space.inline_size, style.max_width(), constraints)
            };
        let max_inline = min_inline.max(specified_max_inline);
        let min_block = if is_plain_auto(style.min_height()) {
            CssPixels::default()
        } else {
            self.calculate_inner_block_size(sized, available_space, style.min_height(), constraints)
        };
        let specified_max_block =
            if self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
                max_dimension_value()
            } else {
                self.calculate_inner_block_size(sized, available_space, style.max_height(), constraints)
            };
        let max_block = min_block.max(specified_max_block);

        // These are from the "Constraint Violation" table in spec, but reordered so that each condition is
        // interpreted as mutually exclusive to any other.
        if input_inline_size < min_inline && input_block_size > max_block {
            return (min_inline, max_block);
        }
        if input_inline_size > max_inline && input_block_size < min_block {
            return (max_inline, min_block);
        }
        if input_inline_size > CssPixels::default() && input_block_size > CssPixels::default() {
            let max_inline_fraction_le_max_block = (max_inline.raw_value() as i64)
                * (input_block_size.raw_value() as i64)
                <= (max_block.raw_value() as i64) * (input_inline_size.raw_value() as i64);
            if input_inline_size > max_inline && input_block_size > max_block && max_inline_fraction_le_max_block {
                return (
                    max_inline,
                    min_block.max(self.content_block_size_from_aspect_ratio(sized, max_inline)),
                );
            }
            if input_inline_size > max_inline && input_block_size > max_block && !max_inline_fraction_le_max_block {
                return (
                    min_inline.max(self.content_inline_size_from_aspect_ratio(sized, max_block)),
                    max_block,
                );
            }
            let min_inline_fraction_le_min_block = (min_inline.raw_value() as i64)
                * (input_block_size.raw_value() as i64)
                <= (min_block.raw_value() as i64) * (input_inline_size.raw_value() as i64);
            if input_inline_size < min_inline && input_block_size < min_block && min_inline_fraction_le_min_block {
                return (
                    max_inline.min(self.content_inline_size_from_aspect_ratio(sized, min_block)),
                    min_block,
                );
            }
            if input_inline_size < min_inline && input_block_size < min_block && !min_inline_fraction_le_min_block {
                return (
                    min_inline,
                    max_block.min(self.content_block_size_from_aspect_ratio(sized, min_inline)),
                );
            }
        }
        if input_inline_size > max_inline {
            return (
                max_inline,
                self.content_block_size_from_aspect_ratio(sized, max_inline)
                    .max(min_block),
            );
        }
        if input_inline_size < min_inline {
            return (
                min_inline,
                self.content_block_size_from_aspect_ratio(sized, min_inline)
                    .min(max_block),
            );
        }
        if input_block_size > max_block {
            return (
                self.content_inline_size_from_aspect_ratio(sized, max_block)
                    .max(min_inline),
                max_block,
            );
        }
        if input_block_size < min_block {
            return (
                self.content_inline_size_from_aspect_ratio(sized, min_block)
                    .min(max_inline),
                min_block,
            );
        }
        (input_inline_size, input_block_size)
    }

    // The computed inline and block sizes a replaced element is sized from, with sizes that must be
    // treated as automatic replaced by `auto`.
    fn computed_sizes_for_replaced_element(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> (&'pass ComputedSize, &'pass ComputedSize) {
        let node = sized.node;
        let style = self.style(node);
        // NB: A size that is auto itself keeps its value, as does a calc-size(), which resolves against the size its
        //     basis gives.
        let inline = if style.width().is_auto() || style.width().calc_size().is_some() {
            style.width()
        } else if self.should_treat_inline_size_as_auto(sized, available_space) {
            auto_computed_size()
        } else {
            style.width()
        };
        let preferred_block_size = self.preferred_block_size(node, constraints);
        let block = if preferred_block_size.is_auto() || preferred_block_size.calc_size().is_some() {
            preferred_block_size
        } else if self.should_treat_block_size_as_auto(sized, available_space, constraints) {
            auto_computed_size()
        } else {
            preferred_block_size
        };
        (inline, block)
    }

    pub(crate) fn compute_inline_size_for_replaced_element(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // 10.3.4 Block-level, replaced elements in normal flow...
        // 10.3.2 Inline, replaced elements
        let style = self.style(node);
        let (computed_inline, computed_block) =
            self.computed_sizes_for_replaced_element(sized, available_space, constraints);
        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut used =
            self.tentative_inline_size_for_replaced_element(sized, computed_inline, available_space, constraints);
        if is_plain_auto(computed_inline)
            && is_plain_auto(computed_block)
            && self.facts(node).has_preferred_aspect_ratio()
        {
            let block =
                self.tentative_block_size_for_replaced_element(sized, computed_block, available_space, constraints);
            used = self
                .solve_replaced_size_constraint(sized, used, block, available_space, constraints)
                .0;
        }
        // NB: When the width is automatic and the box's min-content and max-content inline sizes are both transferred
        //     from its block size, an intrinsic size keyword minimum or maximum resolves to the size computed here and
        //     cannot change it. Resolving one would compute this size again.
        let is_transferred_intrinsic_limit = |size: &ComputedSize| {
            computed_inline.is_auto()
                && size.is_intrinsic_sizing_constraint()
                && self.intrinsic_inline_sizes_are_transferred(sized, constraints)
        };
        // NB: The min-content and max-content inline sizes of a replaced element follow a non-automatic block size
        //     through the aspect ratio.
        let transferred_intrinsic_limit = |size: &ComputedSize| {
            (size.is_intrinsic_sizing_constraint()
                && self.facts(node).has_preferred_aspect_ratio()
                && !self.preferred_block_size(node, constraints).is_auto()
                && !self.should_treat_block_size_as_auto(sized, available_space, constraints))
            .then(|| {
                let block_size = self.compute_block_size_for_replaced_element(sized, available_space, constraints);
                self.content_inline_size_from_aspect_ratio(sized, block_size)
            })
        };
        // 2. If the tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints)
            && !is_transferred_intrinsic_limit(style.max_width())
        {
            if let Some(max) = transferred_intrinsic_limit(style.max_width()) {
                used = used.min(max);
            } else {
                let max = self.calculate_inner_inline_size(
                    sized,
                    available_space.inline_size,
                    style.max_width(),
                    constraints,
                );
                if used > max {
                    used = self.tentative_inline_size_for_replaced_element(
                        sized,
                        style.max_width(),
                        available_space,
                        constraints,
                    );
                }
            }
        }
        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        if !is_plain_auto(style.min_width()) && !is_transferred_intrinsic_limit(style.min_width()) {
            if let Some(min) = transferred_intrinsic_limit(style.min_width()) {
                used = used.max(min);
            } else {
                let min = self.calculate_inner_inline_size(
                    sized,
                    available_space.inline_size,
                    style.min_width(),
                    constraints,
                );
                if used < min {
                    used = self.tentative_inline_size_for_replaced_element(
                        sized,
                        style.min_width(),
                        available_space,
                        constraints,
                    );
                }
            }
        }
        used
    }

    pub(crate) fn compute_block_size_for_replaced_element(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // 10.6.2 Inline replaced elements
        // 10.6.4 Block-level replaced elements in normal flow
        // 10.6.6 Floating replaced elements
        // 10.6.10 'inline-block' replaced elements in normal flow
        let style = self.style(node);
        let (computed_inline, computed_block) =
            self.computed_sizes_for_replaced_element(sized, available_space, constraints);
        // 1. The tentative used height is calculated (without 'min-height' and 'max-height')
        let mut used =
            self.tentative_block_size_for_replaced_element(sized, computed_block, available_space, constraints);
        let facts = self.facts(node);
        // NB: A non-replaced box sized through its aspect ratio already has its used inline size, which transfers
        //     through the ratio before its min/max block sizes apply on their own.
        //     https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
        let inline_size_is_already_used = !facts.is_replaced_box() && sized.metrics().has_definite_inline_size();
        if is_plain_auto(computed_inline)
            && is_plain_auto(computed_block)
            && facts.has_preferred_aspect_ratio()
            && !inline_size_is_already_used
        {
            // However, for replaced elements with both 'width' and 'height' computed as 'auto',
            // use the algorithm under 'Minimum and maximum widths'
            // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
            // to find the used width and height.
            let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
            if intrinsic.width.is_some() || intrinsic.height.is_none() {
                // NOTE: This is a special case where calling tentative_inline_size_for_replaced_element() would call us right back,
                //       and we'd end up in an infinite loop. So we need to handle this case separately.
                let inline = self.tentative_inline_size_for_replaced_element(
                    sized,
                    computed_inline,
                    available_space,
                    constraints,
                );
                used = self
                    .solve_replaced_size_constraint(sized, inline, used, available_space, constraints)
                    .1;
            }
        }
        // 2. If this tentative height is greater than 'max-height', the rules above are applied again,
        //    but this time using the value of 'max-height' as the computed value for 'height'.
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
            let max = self.calculate_inner_block_size(sized, available_space, style.max_height(), constraints);
            if used > max {
                used = self.tentative_block_size_for_replaced_element(
                    sized,
                    style.max_height(),
                    available_space,
                    constraints,
                );
            }
        }
        // 3. If the resulting height is smaller than 'min-height', the rules above are applied again,
        //    but this time using the value of 'min-height' as the computed value for 'height'.
        if !is_plain_auto(style.min_height()) {
            let min = self.calculate_inner_block_size(sized, available_space, style.min_height(), constraints);
            if used < min {
                used = self.tentative_block_size_for_replaced_element(
                    sized,
                    style.min_height(),
                    available_space,
                    constraints,
                );
            }
        }
        used
    }

    pub(crate) fn box_is_sized_as_replaced_element(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let node = sized.node;
        let facts = self.facts(node);
        // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a
        // replaced element with a natural aspect ratio and no natural size in that axis, see e.g. CSS2 §10
        // and CSS Flexible Box Model Level 1 §9.2.
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
        if facts.has_replaced_element_table_display_adjustment()
            || (facts.is_replaced_box() && facts.has_auto_content_box_size())
        {
            return true;
        }
        if facts.has_preferred_aspect_ratio() || facts.has_auto_content_box_size() {
            // From CSS2:
            // If height and width both have computed values of auto and the element has an intrinsic ratio but no intrinsic height or width,
            // then the used value of width is undefined in CSS 2.
            // However, it is suggested that, if the containing block’s width does not itself depend on the replaced element’s width,
            // then the used value of width is calculated from the constraint equation used for block-level, non-replaced elements in normal flow.
            //
            // AD-HOC: If box has preferred aspect ratio but width and height are not specified, then we should
            //         size it as a normal box to match other browsers.
            if self.should_treat_inline_size_as_auto(sized, available_space)
                && self.should_treat_block_size_as_auto(sized, available_space, constraints)
                && !facts.auto_content_size().width.is_some()
                && !facts.auto_content_size().height.is_some()
            {
                return false;
            }
            return true;
        }
        false
    }

    fn forwards_button_percentage_block_basis(facts: &NodeFacts<'_>) -> bool {
        facts.is_anonymous_button_content_wrapper() || facts.is_anonymous_button_content_box()
    }

    fn anonymous_button_content_box(&self, button: Node) -> Option<Node> {
        let wrapper = self.first_child(button);
        if wrapper.is_invalid() {
            return None;
        }
        let content_box = self.first_child(wrapper);
        (!content_box.is_invalid() && self.facts(content_box).is_anonymous_button_content_box()).then_some(content_box)
    }

    fn forwards_percentage_bases_as_anonymous_box(&self, node: Node) -> bool {
        let facts = self.facts(node);
        facts.is_box()
            && facts.is_anonymous()
            && !facts.is_table_cell()
            && !facts.has_auto_content_box_size()
            && (!self.state.measures_content_inline_sizes_of(node) || self.is_anonymous_block_box_in_block_flow(node))
    }

    fn is_anonymous_block_box_in_block_flow(&self, node: Node) -> bool {
        let facts = self.facts(node);
        if !facts.is_anonymous() || !facts.is_block_container() || facts.is_table_wrapper() {
            return false;
        }
        let parent = self.callbacks.parent(node);
        if parent.is_invalid() {
            return false;
        }
        let parent_facts = self.facts(parent);
        parent_facts.is_block_container()
            && !parent_facts.children_are_inline()
            && matches!(
                formatting_context::formatting_context_type_created_by_box(parent_facts),
                None | Some(formatting_context::FormattingContextType::Block)
            )
    }

    /// The constraints the children of `containing_block` are laid out with, where the box has the metrics `metrics`
    /// and is laid out with the constraints `constraints`.
    pub(crate) fn constraints_for_child_context(
        &self,
        containing_block: Node,
        metrics: &impl BoxMetricCells,
        constraints: ContainingBlockConstraints,
    ) -> ContainingBlockConstraints {
        let facts = self.facts(containing_block);
        let style = self.style(containing_block);
        // Anonymous boxes are invisible to percentage resolution: their children resolve percentages
        // against the closest non-anonymous ancestor, so an anonymous containing block without a
        // definite size of its own passes the constraints it was given through. Anonymous table
        // cells are the exception: they are proper containing blocks with their own size semantics.
        let should_forward_indefinite_basis = self.forwards_percentage_bases_as_anonymous_box(containing_block);

        let inline = if metrics.has_definite_inline_size() {
            Some(metrics.content_inline_size())
        } else if should_forward_indefinite_basis {
            constraints.percentage_basis_inline_size
        } else {
            None
        };
        // https://quirks.spec.whatwg.org/#the-body-element-fills-the-html-element-quirk
        // INTEROP: Like Chrome, the block size the body fills is definite for its children.
        let quirks_body_block = (facts.document_in_quirks_mode()
            && facts.is_html_body_element()
            && facts.is_block_container()
            && is_plain_auto(style.height())
            && !metrics.has_definite_block_size()
            && !facts.is_absolutely_positioned()
            && !facts.is_floating()
            && !facts.is_inline())
        .then_some(constraints.percentage_basis_block_size)
        .flatten()
        .map(|parent_block_size| {
            let border_box_block_size = parent_block_size - metrics.margin_top() - metrics.margin_bottom();
            (border_box_block_size
                - style.border_top_width()
                - metrics.padding_top()
                - metrics.padding_bottom()
                - style.border_bottom_width())
            .max(CssPixels::default())
        });
        let forwards_button_content_block_basis = Self::forwards_button_percentage_block_basis(&facts);
        // https://www.w3.org/TR/CSS22/visuren.html#anonymous-block-level
        // Anonymous block boxes are ignored when resolving percentage values that would refer to it:
        // the closest non-anonymous ancestor box is used instead.
        // NB: The anonymous boxes inside a button can acquire definite sizes that differ from the button's, e.g. a
        //     post-flexing size of the content box. Their descendants must still resolve percentage heights against
        //     the button's content box.
        let block = if forwards_button_content_block_basis {
            if metrics.has_definite_block_size_only_for_button_content_alignment() {
                // https://www.w3.org/TR/CSS22/visudet.html#the-height-property
                // A min-height does not make an automatic height definite for percentages, even though the button
                // uses it to center its content.
                None
            } else {
                constraints.percentage_basis_block_size
            }
        } else if metrics.has_definite_block_size() {
            Some(metrics.content_block_size())
        } else if quirks_body_block.is_some() {
            quirks_body_block
        } else if should_forward_indefinite_basis {
            constraints.percentage_basis_block_size
        } else {
            None
        };

        // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
        // 1. Let element be the nearest ancestor containing block of element, if there is one.
        //    Otherwise, return the initial containing block.
        //
        // 2. If element has a computed value of the display property that is table-cell, then return a
        //    UA-defined value.
        // FIXME: Likely UA-defined value should not be 0.
        //
        // 3. If element has a computed value of the height property that is not auto, then return element.
        //
        // 4. If element has a computed value of the position property that is absolute, or if element is a
        //    not a block container or a table wrapper box, then return element.
        //
        // 5. Jump to the first step.
        // NOTE: Evaluated incrementally: in-flow auto-height block containers pass the basis they
        //       inherited from their own containing block through to their children.
        let quirks_block = if !facts.document_in_quirks_mode() {
            None
        } else if facts.is_viewport() {
            Some(metrics.content_block_size())
        } else if facts.is_table_cell() {
            Some(CssPixels::default())
        } else if quirks_body_block.is_some() {
            quirks_body_block
        } else if !style.height().is_auto()
            || facts.is_absolutely_positioned()
            || !facts.is_block_container()
            || facts.is_table_wrapper()
        {
            Some(metrics.content_block_size())
        } else if facts.lays_out_in_columns() {
            None
        } else if facts.parent_is_flex_or_grid_container() && metrics.has_definite_block_size() {
            Some(metrics.content_block_size())
        } else {
            constraints.quirks_mode_percentage_basis_block_size
        };

        let forwarded_a_block_basis = (((!metrics.has_definite_block_size() && should_forward_indefinite_basis)
            || forwards_button_content_block_basis)
            && constraints.percentage_basis_block_size.is_some())
            || (quirks_block.is_some()
                && quirks_block == constraints.quirks_mode_percentage_basis_block_size
                && facts.document_in_quirks_mode()
                && !facts.is_viewport()
                && !facts.is_table_cell()
                && style.height().is_auto()
                && !facts.is_absolutely_positioned()
                && facts.is_block_container()
                && !facts.is_table_wrapper());
        debug_assert!(
            !forwarded_a_block_basis || self.passes_percentage_block_size_through_to_children(containing_block),
            "a containing block that forwards a percentage block basis must report it to dependency tracking"
        );

        ContainingBlockConstraints {
            percentage_basis_inline_size: inline,
            percentage_basis_block_size: block,
            quirks_mode_percentage_basis_block_size: quirks_block,
        }
    }

    pub(crate) fn own_style_depends_on_percentage_block_size(&self, node: Node) -> bool {
        let facts = self.facts(node);
        if facts.is_text_node() {
            return false;
        }
        let style = self.style(node);
        let size_reads_block_axis_input = |size: &ComputedSize| size.contains_percentage() || size.is_fit_content();
        let quirks_fill_viewport_rule_reads_block_basis = facts.document_in_quirks_mode()
            && style.height().is_auto()
            && (facts.is_html_html_element() || facts.is_html_body_element());
        let table_cell_vertical_padding_resolves_against_block_basis = facts.is_table_cell()
            && (style.padding_top().contains_percentage() || style.padding_bottom().contains_percentage());
        size_reads_block_axis_input(style.height())
            || size_reads_block_axis_input(style.min_height())
            || size_reads_block_axis_input(style.max_height())
            || quirks_fill_viewport_rule_reads_block_basis
            || table_cell_vertical_padding_resolves_against_block_basis
    }

    pub(crate) fn passes_percentage_block_size_through_to_children(&self, node: Node) -> bool {
        let facts = self.facts(node);
        if facts.is_text_node() {
            return false;
        }
        let table_formatting_context_resolves_participants_against_its_input_basis =
            facts.is_table_wrapper() || facts.is_table_box();
        if table_formatting_context_resolves_participants_against_its_input_basis {
            return true;
        }
        let style = self.style(node);
        let in_quirks_mode = facts.document_in_quirks_mode();
        let svg_root_forwards_quirks_basis = facts.is_svg_svg_box() && in_quirks_mode;
        let forwards_block_basis_as_anonymous_box = self.forwards_percentage_bases_as_anonymous_box(node);
        let forwards_quirks_basis_as_auto_height_block_container = in_quirks_mode
            && !facts.is_viewport()
            && !facts.is_table_cell()
            && style.height().is_auto()
            && !facts.is_absolutely_positioned()
            && facts.is_block_container()
            && !facts.is_table_wrapper();
        svg_root_forwards_quirks_basis
            || Self::forwards_button_percentage_block_basis(&facts)
            || forwards_block_basis_as_anonymous_box
            || forwards_quirks_basis_as_auto_height_block_container
    }

    pub(crate) fn resolve_percentage_block_size_dependency(&self, node: Node) -> bool {
        let found = self.state.percentage_block_size_dependency_of(node);
        let depends_on_percentage_block_size = found.depends_on_percentage_block_size
            || self.own_style_depends_on_percentage_block_size(node)
            || (found.has_descendant_that_depends_on_percentage_block_size
                && self.passes_percentage_block_size_through_to_children(node));
        if depends_on_percentage_block_size {
            self.state.note_box_depends_on_percentage_block_size(node);
        }
        depends_on_percentage_block_size
    }

    pub(crate) fn skipped_child_depends_on_percentage_block_size(&self, node: Node) -> bool {
        let unvisited_content_below_a_forwarding_box_is_assumed_dependent =
            self.has_children(node) && self.passes_percentage_block_size_through_to_children(node);
        self.own_style_depends_on_percentage_block_size(node)
            || unvisited_content_below_a_forwarding_box_is_assumed_dependent
    }

    // Descendants resolve percentages against the measured box itself, so only the box's own style
    // can reach its containing block's inline basis.
    pub(crate) fn measurement_root_observes_percentage_inline_basis(&self, node: Node) -> bool {
        let facts = self.facts(node);
        if facts.is_list_item_marker_box() {
            return self.list_item_marker_observes_percentage_inline_basis(node);
        }
        if facts.is_anonymous() || facts.is_replaced_box() || facts.has_auto_content_box_size() {
            return true;
        }
        Self::own_box_resolves_percentage_against_inline_basis(self.style(node))
    }

    fn own_box_resolves_percentage_against_inline_basis(style: StyleValues<'_>) -> bool {
        style.width().contains_percentage()
            || style.min_width().contains_percentage()
            || style.max_width().contains_percentage()
            || Self::box_edges_contain_percentage(style)
    }

    fn list_item_marker_observes_percentage_inline_basis(&self, marker: Node) -> bool {
        let style = self.style(marker);
        if Self::own_box_resolves_percentage_against_inline_basis(style) {
            return true;
        }
        let marker_content_may_read_its_containing_block_inline_size = style.text_indent().contains_percentage()
            || self
                .callbacks
                .children(marker)
                .any(|content| self.facts(content).is_replaced_box());
        marker_content_may_read_its_containing_block_inline_size && self.is_anonymous_block_box_in_block_flow(marker)
    }

    fn box_edges_contain_percentage(style: StyleValues<'_>) -> bool {
        style.padding_left().contains_percentage()
            || style.padding_right().contains_percentage()
            || style.padding_top().contains_percentage()
            || style.padding_bottom().contains_percentage()
            || style.margin_left().contains_percentage()
            || style.margin_right().contains_percentage()
            || style.margin_top().contains_percentage()
            || style.margin_bottom().contains_percentage()
    }

    pub(crate) fn charge_measurement_dependency_to_measured_box_and_containing_block(
        &self,
        node: Node,
        depends_on_percentage_block_size: bool,
    ) {
        if !depends_on_percentage_block_size {
            return;
        }
        self.state.note_box_depends_on_percentage_block_size(node);
        formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
            self.state,
            &self.callbacks,
            node,
            true,
        );
    }

    pub(crate) fn should_treat_inline_size_as_auto(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
    ) -> bool {
        let node = sized.node;
        let style = self.style(node);
        let size = style.width();
        if size.is_auto() {
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage() {
            match formatting_context::cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                true,
                available_space.inline_size,
                formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                formatting_context::CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    return false;
                }
                formatting_context::CyclicPercentageIntrinsicContribution::TreatAsInitialValue => {
                    return true;
                }
                formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => {}
            }
            if available_space.inline_size == AvailableSize::Indefinite {
                return true;
            }
        }
        let facts = self.facts(node);
        // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for width...
        if facts.has_preferred_aspect_ratio() && size.is_intrinsic_sizing_constraint() {
            // If the box has no natural height to resolve the aspect ratio, we treat the width as auto.
            if !facts.auto_content_size().height.is_some() {
                return true;
            }
            // If the box has definite height, we can resolve the width through the aspect ratio.
            if sized.metrics().has_definite_block_size() {
                return true;
            }
        }
        false
    }

    // The computed `height`, or `auto` where a percentage of it is treated as `auto`.
    pub(crate) fn preferred_block_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &'pass ComputedSize {
        let size = self.style(node).height();
        if !size.contains_percentage() {
            return size;
        }
        let facts = self.facts(node);
        // https://www.w3.org/TR/CSS22/visudet.html#the-height-property
        // If the height of the containing block is not specified explicitly (i.e., it depends on
        // content height), and this element is not absolutely positioned, the percentage value
        // is treated as 'auto'.
        // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
        // In quirks mode, percentage heights can resolve even without explicit containing block
        // height. The quirk applies to DOM elements only (not anonymous boxes), and excludes
        // table-related display types.
        if !facts.is_absolutely_positioned() {
            // Flex/grid items resolve percentage heights against their container, not via quirk.
            // The quirk should not apply inside user agent shadow trees.
            let quirk_applies = facts.document_in_quirks_mode()
                && constraints.quirks_mode_percentage_basis_block_size.is_some()
                && !facts.is_anonymous()
                && !facts.is_table_box()
                && !facts.parent_is_flex_or_grid_container()
                && !facts.is_in_user_agent_shadow_tree();
            if !quirk_applies && constraints.percentage_basis_block_size.is_none() {
                return auto_computed_size();
            }
        }
        size
    }

    // NB: Like auto, a definite inline size transfers through the aspect ratio of a non-replaced box.
    pub(crate) fn block_size_transfers_through_aspect_ratio(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let node = sized.node;
        let facts = self.facts(node);
        sized.metrics().has_definite_inline_size()
            && !facts.is_replaced_box()
            && facts.has_preferred_aspect_ratio()
            && self.preferred_block_size(node, constraints).is_auto()
    }

    pub(crate) fn should_treat_block_size_as_auto(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let node = sized.node;
        let style = self.style(node);
        let size = style.height();
        let facts = self.facts(node);
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage() {
            match formatting_context::cyclic_percentage_intrinsic_contribution(
                facts.is_replaced_box(),
                true,
                available_space.block_size,
                formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                formatting_context::CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    // AD-HOC: The spec resolves a replaced box's cyclic percentage against zero for its min-content
                    //         contribution in either axis. But a percentage height with nothing to resolve against is
                    //         auto here, as it is everywhere else, for web compatibility — so the box takes its block
                    //         size from its aspect ratio or natural size, as with height: auto.
                    if !self.preferred_block_size(node, constraints).is_auto() {
                        return false;
                    }
                }
                formatting_context::CyclicPercentageIntrinsicContribution::TreatAsInitialValue => {
                    return !self.block_size_transfers_through_aspect_ratio(sized, constraints);
                }
                formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => {}
            }
        }
        if self.preferred_block_size(node, constraints).is_auto() {
            if sized.metrics().has_definite_inline_size() && facts.has_preferred_aspect_ratio() {
                return false;
            }
            return true;
        }
        // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for height...
        if facts.has_preferred_aspect_ratio() && size.is_intrinsic_sizing_constraint() {
            // If the box has no natural width to resolve the aspect ratio, we treat the height as auto.
            if !facts.auto_content_size().width.is_some() {
                return true;
            }
            // If the box has definite width, we can resolve the height through the aspect ratio.
            if sized.metrics().has_definite_inline_size() {
                return true;
            }
        }
        false
    }

    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
    /// The limits that the block axis min/max sizes of a box with a preferred aspect ratio and an automatic inline size
    /// transfer to its inline size. Apply them after `max-width` and before `min-width`, so that the explicit limits win.
    pub(crate) fn transferred_inline_size_limits(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> Option<TransferredInlineSizeLimits> {
        let node = sized.node;
        let facts = self.facts(node);
        // NB: Replaced boxes transfer their limits as part of replaced element sizing.
        if facts.is_replaced_box() || !self.should_treat_inline_size_as_auto(sized, available_space) {
            return None;
        }
        let ratio = facts.preferred_aspect_ratio()?;
        let style = self.style(node);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        // NB: The block axis padding may not be resolved into the used values yet, so it is resolved from the style.
        let definite_block_size = |size: &ComputedSize| -> Option<CssPixels> {
            if !size.is_length_percentage()
                || (size.contains_percentage() && constraints.percentage_basis_block_size.is_none())
            {
                return None;
            }
            let value = size.to_px(constraints.block_basis());
            if style.box_sizing() != box_sizing::BORDER_BOX {
                return Some(value);
            }
            Some(formatting_context::subtract_border_box_adjustment(
                value,
                style.border_top_width(),
                style.padding_top().to_px(containing_block_inline_size),
                style.border_bottom_width(),
                style.padding_bottom().to_px(containing_block_inline_size),
            ))
        };
        let to_inline_size = |block_size: CssPixels| {
            formatting_context::content_inline_size_from_aspect_ratio_values(
                block_size,
                ratio,
                style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
                style.border_left_width() + style.padding_left().to_px(containing_block_inline_size),
                style.border_right_width() + style.padding_right().to_px(containing_block_inline_size),
                style.border_top_width() + style.padding_top().to_px(containing_block_inline_size),
                style.border_bottom_width() + style.padding_bottom().to_px(containing_block_inline_size),
            )
        };

        // First, any definite minimum size is converted and transferred from the origin to destination axis. This
        // transferred minimum is capped by any definite preferred or maximum size in the destination axis.
        let mut minimum = to_inline_size(definite_block_size(style.min_height()).unwrap_or_default());
        if !self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            minimum = minimum.min(self.calculate_inner_inline_size(
                sized,
                available_space.inline_size,
                style.max_width(),
                constraints,
            ));
        }
        // Then, any definite maximum size is converted and transferred from origin to destination. This transferred
        // maximum is floored by any definite preferred or minimum size in the destination axis as well as by the
        // transferred minimum, if any.
        // NB: The explicit minimum is applied after these limits, so it floors the transferred maximum there.
        let maximum = if self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
            None
        } else {
            definite_block_size(style.max_height()).map(|maximum| to_inline_size(maximum).max(minimum))
        };
        Some(TransferredInlineSizeLimits { minimum, maximum })
    }

    pub(crate) fn should_treat_max_inline_size_as_none(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let size = self.style(node).max_width();
        // NB: A keyword limit is no limit under the same intrinsic constraint, unless calc-size() changes its size.
        let keyword_limit_is_cyclic = size.calc_size().is_none();
        if size.is_none()
            || (available == AvailableSize::MaxContent && size.is_max_content() && keyword_limit_is_cyclic)
        {
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage() {
            match formatting_context::cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                true,
                available,
                formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                formatting_context::CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    return false;
                }
                formatting_context::CyclicPercentageIntrinsicContribution::TreatAsInitialValue => {
                    return true;
                }
                formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => {}
            }
            if constraints.percentage_basis_inline_size.is_none() {
                return true;
            }
        }
        keyword_limit_is_cyclic
            && ((size.is_fit_content() && available.is_intrinsic_sizing_constraint())
                || (size.is_max_content() && available == AvailableSize::MaxContent)
                || (size.is_min_content() && available == AvailableSize::MinContent))
    }

    pub(crate) fn should_treat_max_block_size_as_none(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        // https://www.w3.org/TR/CSS22/visudet.html#min-max-heights
        // If the height of the containing block is not specified explicitly (i.e., it depends on content height),
        // and this element is not absolutely positioned, the percentage value is treated as '0' (for 'min-height')
        // or 'none' (for 'max-height').
        let size = self.style(node).max_height();
        if size.is_none() {
            return true;
        }
        if size.contains_percentage() && constraints.percentage_basis_block_size.is_none() {
            return true;
        }
        // NB: As for the inline axis, calc-size() can change the size of a keyword limit.
        let keyword_limit_is_cyclic = size.calc_size().is_none();
        keyword_limit_is_cyclic
            && ((size.is_fit_content() && available.is_intrinsic_sizing_constraint())
                || (size.is_max_content() && available == AvailableSize::MaxContent))
    }

    // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
    // The available space to resolve a block-level box's block size against:
    // in quirks mode, percentage heights outside table internals and UA
    // shadow trees resolve against the quirks percentage basis.
    pub(crate) fn available_space_for_block_size_resolution(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> AvailableSpace {
        let facts = self.facts(node);
        // The quirks-mode check comes first so standards-mode documents pay a
        // single flag read here.
        if !facts.document_in_quirks_mode()
            || !self.style(node).height().is_percentage()
            || facts.is_in_user_agent_shadow_tree()
        {
            return available_space;
        }
        let is_table_box = facts.is_table_row()
            || facts.is_table_row_group()
            || facts.is_table_header_group()
            || facts.is_table_footer_group()
            || facts.is_table_cell()
            || facts.is_table_caption();
        if is_table_box {
            return available_space;
        }
        let Some(quirks_mode_percentage_basis_block_size) = constraints.quirks_mode_percentage_basis_block_size else {
            return available_space;
        };
        let mut resolution_space = available_space;
        resolution_space.block_size = AvailableSize::definite(quirks_mode_percentage_basis_block_size);
        resolution_space
    }

    pub(crate) fn clamp_block_size_to_min_max(
        &self,
        sized: SizedBox<'_>,
        mut block_size: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style(node);
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            let max = self.calculate_inner_block_size(sized, available_space, style.max_height(), constraints);
            block_size = block_size.min(max);
        }
        if !is_plain_auto(style.min_height()) {
            let min = self.calculate_inner_block_size(sized, available_space, style.min_height(), constraints);
            block_size = block_size.max(min);
        }
        block_size
    }

    fn has_automatic_minimum_size_from_aspect_ratio(&self, node: Node, min_size: &ComputedSize) -> bool {
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
        // In order to avoid unintentional overflow, the automatic minimum size in the ratio-dependent axis of a box
        // with a preferred aspect ratio that is neither a replaced element nor a scroll container is its min-content
        // size capped by its maximum size.
        let facts = self.facts(node);
        min_size.is_auto()
            && facts.has_preferred_aspect_ratio()
            && !facts.is_replaced_box()
            && !facts.is_scroll_container()
    }

    /// The automatic minimum size of a box in the ratio-dependent axis: its min-content size capped by its maximum size.
    /// Without a known content size, the content is measured with the box at its transferred size.
    #[expect(clippy::too_many_arguments)]
    pub(crate) fn automatic_minimum_size_from_aspect_ratio(
        &self,
        sized: SizedBox<'_>,
        axis: SizingAxis,
        other_axis_size: CssPixels,
        transferred_size: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        content_size: Option<CssPixels>,
    ) -> Option<CssPixels> {
        let node = sized.node;
        let style = self.style(node);
        let (preferred_size, min_size) = match axis {
            SizingAxis::Inline => (style.width(), style.min_width()),
            SizingAxis::Block => (style.height(), style.min_height()),
        };
        // INTEROP: Other engines only apply this minimum when the preferred size is auto, not when it is a percentage
        //          that is treated as auto.
        if !preferred_size.is_auto() || !self.has_automatic_minimum_size_from_aspect_ratio(node, min_size) {
            return None;
        }
        let minimum = content_size.unwrap_or_else(|| {
            self.min_content_size_of_aspect_ratio_box_content(
                node,
                axis,
                other_axis_size,
                transferred_size,
                constraints,
            )
        });
        // NB: A calc-size() with an auto basis resolves against the automatic minimum size.
        let percentage_basis = match axis {
            SizingAxis::Inline => constraints.percentage_basis_inline_size,
            SizingAxis::Block => constraints.percentage_basis_block_size,
        };
        let minimum = self.resolve_calc_size(sized, axis, min_size, minimum, percentage_basis);
        // NB: An intrinsic size keyword maximum is at least the box's min-content size, which is no smaller than this
        //     minimum, so it never caps it. Resolving it would measure the box's intrinsic size, which uses this minimum.
        let maximum = match axis {
            SizingAxis::Inline => {
                (!self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints)
                    && !style.max_width().is_auto()
                    && !style.max_width().is_intrinsic_sizing_constraint())
                .then(|| {
                    self.calculate_inner_inline_size(sized, available_space.inline_size, style.max_width(), constraints)
                })
            }
            SizingAxis::Block => {
                (!self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
                    && !style.max_height().is_auto())
                .then(|| self.calculate_inner_block_size(sized, available_space, style.max_height(), constraints))
            }
        };
        Some(maximum.map_or(minimum, |maximum| minimum.min(maximum)))
    }

    fn min_content_size_of_aspect_ratio_box_content(
        &self,
        node: Node,
        axis: SizingAxis,
        other_axis_size: CssPixels,
        transferred_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // NB: The box's own intrinsic sizes are transferred through the ratio, so measure its content instead.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        if axis == SizingAxis::Inline {
            return self
                .measure_intrinsic_inline_size(
                    node,
                    constraints,
                    Some(other_axis_size),
                    IntrinsicSizeCacheKind::MinContentInline,
                )
                .size;
        }
        // NB: Percentages inside resolve against the transferred block size, which is definite.
        if let Some(content_box) = self.anonymous_button_content_box(node) {
            // The anonymous wrapper fills the button, so measure the content box, which forwards the button's size as
            // the basis for percentages.
            let mut content_constraints = constraints;
            content_constraints.percentage_basis_inline_size = Some(other_axis_size);
            content_constraints.percentage_basis_block_size = Some(transferred_size);
            if self.establishes_orthogonal_flow(content_box) {
                // NB: A run that sizes the button never holds its content box, whose sizing reads no metrics of it.
                return self.content_block_size_of_orthogonal_flow(
                    SizedBox::unheld(content_box),
                    other_axis_size,
                    content_constraints,
                    IntrinsicSizeCacheKind::MaxContentInline,
                );
            }
            return self.measure_intrinsic_block_size(content_box, other_axis_size, content_constraints);
        }
        let mut root = used_values::cells_before_sizing(&self.callbacks, node, constraints);
        root.set_content_inline_size(other_axis_size);
        root.set_content_block_size(transferred_size);
        root.has_definite_block_size = true;
        let result = formatting_context::lay_out_measurement_root_for_its_result(
            self.callbacks,
            node,
            root,
            LayoutInput::new(
                AvailableSpace {
                    inline_size: AvailableSize::definite(other_axis_size),
                    block_size: AvailableSize::definite(transferred_size),
                },
                constraints,
                ParticipationInParentFormattingContext::Root,
            ),
        );
        result
            .content_block_size_for_aspect_ratio_minimum
            .unwrap_or(result.automatic_content_block_size)
    }

    /// Whether the block size of this box is its ratio-dependent axis, i.e. transferred from its inline size.
    pub(crate) fn block_size_is_ratio_dependent(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let node = sized.node;
        self.style(node).height().is_auto()
            && self.has_automatic_minimum_size_from_aspect_ratio(node, self.style(node).min_height())
            && !self.should_treat_block_size_as_auto(sized, available_space, constraints)
    }

    /// Grows a block size that was transferred through the preferred aspect ratio to the automatic minimum size, once the
    /// box's content block size is known. Without a known content block size, the content is measured.
    pub(crate) fn apply_automatic_minimum_block_size_from_aspect_ratio(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        content_block_size: Option<CssPixels>,
    ) {
        let sized = sized_cells.sized_box();
        if !self.block_size_is_ratio_dependent(sized, available_space, constraints) {
            return;
        }
        let Some(minimum) = self.automatic_minimum_size_from_aspect_ratio(
            sized,
            SizingAxis::Block,
            sized_cells.cells.content_inline_size(),
            sized_cells.cells.content_block_size(),
            available_space,
            constraints,
            content_block_size,
        ) else {
            return;
        };
        let used = sized_cells.cells;
        if used.content_block_size() < minimum {
            used.set_content_block_size(minimum);
        }
    }

    pub(crate) fn resolve_used_block_size_if_not_treated_as_auto(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        let sized = sized_cells.sized_box();
        if let Some(resolved) = self.used_block_size_if_not_treated_as_auto(sized, available_space, constraints) {
            sized_cells.cells.update_cells(|cells| resolved.apply_to_cells(cells));
        }
    }

    /// The block size of `node` where its preferred block size is not treated as auto.
    pub(crate) fn used_block_size_if_not_treated_as_auto(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> Option<ResolvedBlockSize> {
        let node = sized.node;
        if self.should_treat_block_size_as_auto(sized, available_space, constraints) {
            return None;
        }
        let style = self.style(node);
        let block_size = self.calculate_inner_block_size(
            sized,
            available_space,
            self.preferred_block_size(node, constraints),
            constraints,
        );
        Some(ResolvedBlockSize {
            content_block_size: self.clamp_block_size_to_min_max(sized, block_size, available_space, constraints),
            makes_block_size_definite: !style.height().is_intrinsic_sizing_constraint(),
        })
    }

    pub(crate) fn resolve_used_block_size_if_treated_as_auto(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        child_automatic_block_size: Option<CssPixels>,
        automatic_block_size_fallback: impl FnOnce() -> CssPixels,
    ) {
        let sized = sized_cells.sized_box();
        if let Some(resolved) = self.used_block_size_if_treated_as_auto(
            sized,
            available_space,
            constraints,
            child_automatic_block_size,
            automatic_block_size_fallback,
        ) {
            sized_cells.cells.update_cells(|cells| resolved.apply_to_cells(cells));
        }
    }

    /// The block size of `node` where its preferred block size is treated as auto: its automatic block size, given as
    /// `child_automatic_block_size` or found by `automatic_block_size_fallback`, unless it is sized as a replaced box.
    pub(crate) fn used_block_size_if_treated_as_auto(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        child_automatic_block_size: Option<CssPixels>,
        automatic_block_size_fallback: impl FnOnce() -> CssPixels,
    ) -> Option<ResolvedBlockSize> {
        let node = sized.node;
        if !self.should_treat_block_size_as_auto(sized, available_space, constraints) {
            return None;
        }
        let style = self.style(node);
        let facts = self.facts(node);
        let box_is_sized_as_replaced_element =
            self.box_is_sized_as_replaced_element(sized, available_space, constraints);
        let mut makes_block_size_definite = sized.metrics().has_definite_inline_size()
            && facts.has_preferred_aspect_ratio()
            && box_is_sized_as_replaced_element;
        // NB: Replaced sizing resolves a calc-size() itself.
        let mut block_size = if box_is_sized_as_replaced_element {
            self.compute_block_size_for_replaced_element(sized, available_space, constraints)
        } else {
            self.resolve_calc_size(
                sized,
                SizingAxis::Block,
                style.height(),
                child_automatic_block_size.unwrap_or_else(automatic_block_size_fallback),
                constraints.percentage_basis_block_size,
            )
        };
        block_size = self.clamp_block_size_to_min_max(sized, block_size, available_space, constraints);

        if facts.document_in_quirks_mode() && facts.is_html_html_element() && is_plain_auto(style.height()) {
            // 3.6. The html element fills the viewport quirk
            // https://quirks.spec.whatwg.org/#the-html-element-fills-the-viewport-quirk
            // FIXME: Handle vertical writing mode.

            // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
            //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
            //    the margin-top and margin-bottom properties of element.
            let metrics = sized.metrics();
            let margins = metrics.margin_top() + metrics.margin_bottom();
            // 2. Let size be the size of the initial containing block in the block flow direction minus margins.
            let size = constraints.block_basis() - margins;
            // 3. Return the bigger value of size and the normal border box size the element would have
            //    according to the CSS specification.
            // NB: block_size is a content box size, so this takes the border and padding off size.
            block_size = block_size.max(size - metrics.border_box_top(false) - metrics.border_box_bottom(false));
            // NOTE: The block size of the root element when affected by this quirk is considered to be definite.
            makes_block_size_definite = true;
        }

        if facts.document_in_quirks_mode() && facts.is_html_body_element() && is_plain_auto(style.height()) {
            // 3.7. The body element fills the html element quirk
            // https://quirks.spec.whatwg.org/#the-body-element-fills-the-html-element-quirk
            // FIXME: Handle vertical writing mode.

            // The element body must additionally meet the following conditions:
            // - The computed value of the 'position' property of element is neither 'absolute' nor 'fixed'.
            // - The computed value of the 'float' property of element is 'none'.
            // - Element is not an inline-level element.
            // - Element is not a multi-column spanning element.
            // NON-STANDARD: We don't check column-span since no browser actually excludes it.
            if !facts.is_absolutely_positioned() && !facts.is_floating() && !facts.is_inline() {
                // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
                //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
                //    the margin-top and margin-bottom properties of element.
                let metrics = sized.metrics();
                let margins = metrics.margin_top() + metrics.margin_bottom();
                // 2. Let size be the size of element's parent element's content box in the block flow direction minus margins.
                let size = constraints.block_basis() - margins;
                // 3. Return the bigger value of size and the normal border box size the element would have
                //    according to the CSS specification.
                // NB: block_size is a content box size, so this takes the border and padding off size.
                block_size = block_size.max(size - metrics.border_box_top(false) - metrics.border_box_bottom(false));
            }
        }
        Some(ResolvedBlockSize {
            content_block_size: block_size,
            makes_block_size_definite,
        })
    }

    pub(crate) fn resolve_box_model_metrics_against_inline_basis(
        &self,
        sized_cells: SizedCells<'_>,
        containing_inline_size: CssPixels,
    ) {
        let node = sized_cells.node;
        let style = self.style(node);
        let used = sized_cells.cells;
        used.update_cells(|cells| cells.resolve_box_model(&style, containing_inline_size));
    }

    /// Sizes an empty atomic block. Where a committing layout takes its metrics from its committed fragment as they
    /// are, returns that fragment.
    pub(crate) fn dimension_empty_atomic_root(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        layout_builds_fragments: bool,
    ) -> Option<std::sync::Arc<fragment_tree::Fragment>> {
        if layout_builds_fragments && let Some(fragment) = self.try_reuse_empty_atomic_root_metrics(sized_cells) {
            return Some(fragment);
        }
        self.dimension_empty_atomic_root_fresh(sized_cells, available_space, constraints);
        None
    }

    fn try_reuse_empty_atomic_root_metrics(
        &self,
        sized_cells: SizedCells<'_>,
    ) -> Option<std::sync::Arc<fragment_tree::Fragment>> {
        let node = sized_cells.node;
        let facts = self.facts(node);
        debug_assert!(
            formatting_context::formatting_context_type_created_by_box(facts)
                == Some(formatting_context::FormattingContextType::Block)
        );
        if facts.data().kind.get() != NodeKind::BlockContainer
            || facts.is_anonymous()
            || facts.is_native_form_control_box()
            || facts.is_html_html_element()
            || facts.is_html_body_element()
            || facts.is_scroll_container()
            || !self.block_root_inline_size_follows_from_style(node)
        {
            return None;
        }
        let style = self.style(node);
        if !(style.width().is_auto() || style.width().is_length())
            || !(style.height().is_auto() || style.height().is_length())
            || !(is_plain_auto(style.min_width()) || style.min_width().is_length())
            || !(is_plain_auto(style.min_height()) || style.min_height().is_length())
            || !(style.max_width().is_none() || style.max_width().is_length())
            || !(style.max_height().is_none() || style.max_height().is_length())
            || Self::box_edges_contain_percentage(style)
        {
            return None;
        }
        let used = sized_cells.cells;
        self.callbacks
            .arena()
            .with_current_committed_fragment(node, |fragment| {
                used.update_cells(|cells| cells.take_box_metrics_of_fragment(fragment));
                fragment.clone()
            })
    }

    fn dimension_empty_atomic_root_fresh(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        let sized = sized_cells.sized_box();
        self.dimension_atomic_root(sized_cells, available_space, constraints, false);
        self.resolve_used_block_size_if_treated_as_auto(
            sized_cells,
            available_space,
            constraints,
            Some(CssPixels::default()),
            || unreachable!("an empty atomic block has a zero automatic content block size"),
        );
        if !self.box_is_sized_as_replaced_element(sized, available_space, constraints) {
            self.resolve_used_block_size_if_not_treated_as_auto(sized_cells, available_space, constraints);
        }
    }

    pub(crate) fn dimension_atomic_root(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        report_for_run_cache_the_available_inline_sizes_this_sizing_repeats_for: bool,
    ) -> Option<CssPixels> {
        let sized = sized_cells.sized_box();
        let node = sized.node;
        let containing_inline_size = available_space.inline_size.to_px_or_zero();
        let style = self.style(node);
        self.resolve_box_model_metrics_against_inline_basis(sized_cells, containing_inline_size);

        let facts = self.facts(node);
        if self.box_is_sized_as_replaced_element(sized, available_space, constraints) {
            let inline_size = self.compute_inline_size_for_replaced_element(sized, available_space, constraints);
            sized_cells.cells.set_content_inline_size(inline_size);

            let block_size = self.compute_block_size_for_replaced_element(sized, available_space, constraints);
            sized_cells.cells.set_content_block_size(block_size);

            // Native form controls lay out their internal shadow contents against the concrete size produced by
            // replaced sizing. That size remains definite when a min/max constraint clamps the automatic preferred
            // height, unless the preferred height is itself an intrinsic sizing constraint.
            let native_control_block_size_constraint_is_active = facts.is_native_form_control_box()
                && !style.height().is_intrinsic_sizing_constraint()
                && facts.auto_content_size().height.is_some_and(|auto_content_height| {
                    (!is_plain_auto(style.min_height()) && block_size > auto_content_height)
                        || (!style.max_height().is_auto()
                            && !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
                            && block_size < auto_content_height)
                });

            let block_size_is_automatic = self.preferred_block_size(node, constraints).is_auto()
                || self.should_treat_block_size_as_auto(sized, available_space, constraints);
            let block_size_is_definite_from_aspect_ratio = sized_cells.cells.has_definite_inline_size()
                && facts.has_preferred_aspect_ratio()
                && block_size_is_automatic;

            if native_control_block_size_constraint_is_active || block_size_is_definite_from_aspect_ratio {
                sized_cells
                    .cells
                    .update_cells(|cells| cells.has_definite_block_size = true);
            }
            return None;
        }

        let resolution = self.calculate_atomic_root_content_inline_size(sized, available_space, constraints);
        sized_cells
            .cells
            .set_content_inline_size(resolution.content_inline_size);
        let sizing_repeats_for_available_inline_sizes_at_or_above =
            report_for_run_cache_the_available_inline_sizes_this_sizing_repeats_for
                .then(|| {
                    self.available_inline_size_at_or_above_which_this_atomic_root_sizing_repeats(
                        sized,
                        available_space,
                        &resolution,
                    )
                })
                .flatten();

        let inline_definite_space = AvailableSpace {
            inline_size: AvailableSize::definite(resolution.content_inline_size),
            block_size: AvailableSize::Indefinite,
        };
        self.resolve_used_block_size_if_not_treated_as_auto(sized_cells, inline_definite_space, constraints);
        self.make_form_control_content_box_definite(sized_cells, available_space, constraints, None);
        sizing_repeats_for_available_inline_sizes_at_or_above
    }

    fn available_inline_size_at_or_above_which_this_atomic_root_sizing_repeats(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        resolution: &AtomicRootInlineSizeResolution,
    ) -> Option<CssPixels> {
        let node = sized.node;
        if !matches!(available_space.inline_size, AvailableSize::Definite(_)) {
            return None;
        }
        let facts = self.facts(node);
        let button_content_is_measured_against_the_raw_available_space = facts.uses_button_layout();
        let aspect_ratio_transfers_the_available_size_between_axes = facts.has_preferred_aspect_ratio();
        let box_model_or_width_resolves_a_percentage_against_the_inline_basis =
            self.measurement_root_observes_percentage_inline_basis(node);
        if button_content_is_measured_against_the_raw_available_space
            || aspect_ratio_transfers_the_available_size_between_axes
            || box_model_or_width_resolves_a_percentage_against_the_inline_basis
        {
            return None;
        }
        let style = self.style(node);
        let percentage_free_length_or_intrinsic_keyword_never_reads_the_available_inline_size =
            |size: &ComputedSize| size.is_length_percentage() || size.is_min_content() || size.is_max_content();
        let width = style.width();
        if !resolution.width_was_treated_as_auto
            && !percentage_free_length_or_intrinsic_keyword_never_reads_the_available_inline_size(width)
        {
            return None;
        }
        let min_width = style.min_width();
        if !min_width.is_auto()
            && !percentage_free_length_or_intrinsic_keyword_never_reads_the_available_inline_size(min_width)
        {
            return None;
        }
        let max_width = style.max_width();
        if !max_width.is_none()
            && !percentage_free_length_or_intrinsic_keyword_never_reads_the_available_inline_size(max_width)
        {
            return None;
        }
        let block_axis_intrinsic_keywords_measure_against_the_available_inline_size =
            style.height().is_intrinsic_sizing_constraint()
                || style.min_height().is_intrinsic_sizing_constraint()
                || style.max_height().is_intrinsic_sizing_constraint();
        if block_axis_intrinsic_keywords_measure_against_the_available_inline_size {
            return None;
        }
        if !resolution.width_was_treated_as_auto {
            let every_definite_available_inline_size = CssPixels::default();
            return Some(every_definite_available_inline_size);
        }
        let max_content_size = resolution.max_content_size_that_fit_the_definite_available_inner_space?;
        Some(max_content_size + sized.metrics().horizontal_margin_border_padding())
    }

    pub(crate) fn calculate_atomic_root_content_inline_size(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> AtomicRootInlineSizeResolution {
        let node = sized.node;
        let style = self.style(node);
        let mut max_content_size_that_fit_the_definite_available_inner_space = None;
        let width_was_treated_as_auto = self.should_treat_inline_size_as_auto(sized, available_space);
        let unconstrained_inline_size = if width_was_treated_as_auto {
            if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                let available =
                    available_space.inline_size.to_px_or_zero() - sized.metrics().horizontal_margin_border_padding();
                let preferred = self.calculate_max_content_inline_size(sized, constraints);
                if preferred <= available {
                    max_content_size_that_fit_the_definite_available_inner_space = Some(preferred);
                    preferred
                } else {
                    self.calculate_min_content_inline_size(sized, constraints)
                        .max(available)
                        .min(preferred)
                }
            } else if available_space.inline_size == AvailableSize::MinContent {
                self.calculate_min_content_inline_size(sized, constraints)
            } else {
                self.calculate_max_content_inline_size(sized, constraints)
            }
        } else if style.width().contains_percentage()
            && !matches!(available_space.inline_size, AvailableSize::Definite(_))
        {
            CssPixels::default()
        } else {
            self.calculate_inner_inline_size(sized, available_space.inline_size, style.width(), constraints)
        };

        let unconstrained_inline_size = if width_was_treated_as_auto {
            self.resolve_calc_size(
                sized,
                SizingAxis::Inline,
                style.width(),
                unconstrained_inline_size,
                constraints.percentage_basis_inline_size,
            )
        } else {
            unconstrained_inline_size
        };
        let mut inline_size = unconstrained_inline_size;
        if !self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            inline_size = inline_size.min(self.calculate_inner_inline_size(
                sized,
                available_space.inline_size,
                style.max_width(),
                constraints,
            ));
        }
        if let Some(limits) = self.transferred_inline_size_limits(sized, available_space, constraints) {
            inline_size = limits.clamp(inline_size);
        }
        if !is_plain_auto(style.min_width()) {
            inline_size = inline_size.max(self.calculate_inner_inline_size(
                sized,
                available_space.inline_size,
                style.min_width(),
                constraints,
            ));
        }
        AtomicRootInlineSizeResolution {
            content_inline_size: inline_size,
            width_was_treated_as_auto,
            max_content_size_that_fit_the_definite_available_inner_space,
        }
    }

    fn block_root_inline_size_follows_from_style(&self, node: Node) -> bool {
        let facts = self.facts(node);
        !self.establishes_orthogonal_flow(node)
            && !facts.has_preferred_aspect_ratio()
            && !facts.has_auto_content_box_size()
            && !facts.uses_button_layout()
            && !facts.is_table_wrapper()
            && !facts.is_fieldset_box()
    }

    pub(crate) fn outer_inline_contributions(
        &self,
        node: Node,
        available_block_size: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> OuterInlineContributions {
        let mut root = used_values::cells_before_sizing(&self.callbacks, node, constraints);
        let indefinite_inline_basis = CssPixels::default();
        root.resolve_box_model(&self.style(node), indefinite_inline_basis);
        let (contributions, percentage_block_size_dependency_its_layout_would_report) =
            RunState::with_content_sizing_root(
                self.callbacks,
                node,
                self.callbacks.in_flow_containing_block(node),
                |state, callbacks| {
                    let sizing = SizingContext::new(state, callbacks);
                    let inline_size_under = |inline_size| {
                        sizing.atomic_root_content_inline_size(
                            SizedBox::of_own_metrics(node, &root, callbacks.frame),
                            AvailableSpace {
                                inline_size,
                                block_size: available_block_size,
                            },
                            constraints,
                        )
                    };
                    let max_content_inline_size = inline_size_under(AvailableSize::MaxContent);
                    let min_content_inline_size = inline_size_under(AvailableSize::MinContent);
                    (
                        OuterInlineContributions {
                            min_content_inline_size,
                            max_content_inline_size,
                            margin_start: root.margin_left,
                            border_start: root.border_left,
                            padding_start: root.padding_left,
                            padding_end: root.padding_right,
                            border_end: root.border_right,
                            margin_end: root.margin_right,
                        },
                        state
                            .percentage_block_size_dependency_of(node)
                            .depends_on_percentage_block_size
                            || sizing.skipped_child_depends_on_percentage_block_size(node),
                    )
                },
            );
        formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
            self.state,
            &self.callbacks,
            node,
            percentage_block_size_dependency_its_layout_would_report,
        );
        contributions
    }

    pub(crate) fn outer_inline_contributions_of_legend_whose_fit_content_size_ignores_its_limits(
        &self,
        sized: SizedBox<'_>,
        available_block_size: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> OuterInlineContributions {
        let legend = sized.node;
        let content_sizes = self.content_inline_sizes(sized, constraints, None).sizes;
        OuterInlineContributions {
            min_content_inline_size: content_sizes.min,
            max_content_inline_size: content_sizes.max,
            ..self.outer_inline_contributions(legend, available_block_size, constraints)
        }
    }

    pub(crate) fn block_level_table_wrapper_outer_inline_contributions(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
    ) -> OuterInlineContributions {
        let wrapper = sized.node;
        let table_border_box_inline_sizes = self.content_inline_sizes(sized, constraints, None).sizes;
        let style = self.style(wrapper);
        let indefinite_inline_basis = CssPixels::default();
        OuterInlineContributions {
            min_content_inline_size: table_border_box_inline_sizes.min,
            max_content_inline_size: table_border_box_inline_sizes.max,
            margin_start: style.margin_left().to_px(indefinite_inline_basis),
            border_start: CssPixels::default(),
            padding_start: CssPixels::default(),
            padding_end: CssPixels::default(),
            border_end: CssPixels::default(),
            margin_end: style.margin_right().to_px(indefinite_inline_basis),
        }
    }

    fn atomic_root_content_inline_size(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        if self.box_is_sized_as_replaced_element(sized, available_space, constraints) {
            return self.compute_inline_size_for_replaced_element(sized, available_space, constraints);
        }
        self.calculate_atomic_root_content_inline_size(sized, available_space, constraints)
            .content_inline_size
    }

    fn calculate_stretch_fit_inline_size(&self, sized: SizedBox<'_>, available: AvailableSize) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
        // The size a box would take if its outer size filled the available space in the given axis;
        // in other words, the stretch fit into the available space, if that is definite.
        //
        // Undefined if the available space is indefinite.
        if !matches!(available, AvailableSize::Definite(_)) {
            return CssPixels::default();
        }
        available.to_px_or_zero() - sized.metrics().horizontal_margin_border_padding()
    }

    /// Whether `node` lays out its contents in a frame orthogonal to the one this context sizes it in.
    pub(crate) fn establishes_orthogonal_flow(&self, node: Node) -> bool {
        formatting_context::frame_established_by(&self.callbacks, node).is_orthogonal_to(self.callbacks.frame)
    }

    fn key_in_frame(&self, mut key: IntrinsicSizeCacheKey) -> IntrinsicSizeCacheKey {
        key.measured_in_transposed_frame = self.callbacks.frame.is_transposed();
        key
    }

    fn intrinsic_block_cache_get(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicBlockSizeMeasurement> {
        let key = self.key_in_frame(key);
        let measurement = self.callbacks.intrinsic_size_caches().intrinsic_block_size_cache_get(
            self.callbacks.intrinsic_size_cache_stamp(node),
            kind,
            key,
        )?;
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            measurement.depends_on_percentage_block_size,
        );
        Some(measurement)
    }

    fn intrinsic_block_cache_put(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicBlockSizeMeasurement,
    ) {
        let key = self.key_in_frame(key);
        self.callbacks.intrinsic_size_caches().intrinsic_block_size_cache_put(
            self.callbacks.intrinsic_size_cache_stamp(node),
            kind,
            key,
            value,
        );
    }

    fn intrinsic_inline_measurement_cache_get(
        &self,
        node: Node,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicInlineSizeMeasurement> {
        let key = self.key_in_frame(key);
        let key = if self.intrinsic_inline_measurement_may_depend_on_block_size(node, key.measured_at_block_size) {
            key
        } else {
            key.with_measured_at_block_size_masked()
        };
        let measurement = self
            .callbacks
            .intrinsic_size_caches()
            .intrinsic_inline_size_measurement_cache_get(self.callbacks.intrinsic_size_cache_stamp(node), key)?;
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            measurement.depends_on_percentage_block_size,
        );
        Some(measurement)
    }

    fn intrinsic_inline_measurement_cache_put(
        &self,
        node: Node,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicInlineSizeMeasurement,
    ) {
        let key = self.key_in_frame(key);
        self.callbacks
            .intrinsic_size_caches()
            .intrinsic_inline_size_measurement_cache_put(self.callbacks.intrinsic_size_cache_stamp(node), key, value);
    }

    fn transferred_intrinsic_inline_sizing_space(&self, sized: SizedBox<'_>) -> AvailableSpace {
        sized
            .metrics()
            .available_inner_space_or_constraints_from(AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: AvailableSize::Indefinite,
            })
    }

    /// Whether the min-content and max-content inline sizes of a box are both its block size transferred through its
    /// preferred aspect ratio.
    fn intrinsic_inline_sizes_are_transferred(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let node = sized.node;
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        // "size constraints in the opposite dimension will transfer through and can affect the auto size in the considered one"
        let facts = self.facts(node);
        let style = self.style(node);
        // https://drafts.csswg.org/css2/#inline-replaced-width
        // "'width' has a computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic ratio"
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
        // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a replaced element
        // with a natural aspect ratio and no natural size in that axis.
        if !facts.has_preferred_aspect_ratio()
            || (facts.is_replaced_box() && !style.width().is_auto())
            || style.height().is_auto()
            || style.height().is_intrinsic_sizing_constraint()
        {
            return false;
        }
        let available_space = self.transferred_intrinsic_inline_sizing_space(sized);
        // NB: A cyclic percentage width of a non-replaced box behaves as auto for its intrinsic sizes.
        self.should_treat_inline_size_as_auto(sized, available_space)
            && !self.should_treat_block_size_as_auto(sized, available_space, constraints)
    }

    fn calculate_transferred_inline_size_for_replaced_element(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        let node = sized.node;
        if !self.intrinsic_inline_sizes_are_transferred(sized, constraints) {
            return None;
        }
        let facts = self.facts(node);
        let available_space = self.transferred_intrinsic_inline_sizing_space(sized);
        // NB: Replaced elements resolve their sizes here without a percentage basis.
        let sizing_constraints = if facts.is_replaced_box() {
            ContainingBlockConstraints::default()
        } else {
            constraints
        };
        // https://drafts.csswg.org/css2/#inline-replaced-width
        // "(used height) * (intrinsic ratio)"
        let inline_size = self.compute_inline_size_for_replaced_element(sized, available_space, sizing_constraints);
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
        let block_size = self.compute_block_size_for_replaced_element(sized, available_space, sizing_constraints);
        let minimum = self.automatic_minimum_size_from_aspect_ratio(
            sized,
            SizingAxis::Inline,
            block_size,
            inline_size,
            available_space,
            constraints,
            None,
        );
        Some(minimum.map_or(inline_size, |minimum| inline_size.max(minimum)))
    }

    pub(crate) fn content_inline_sizes(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> ContentInlineSizes {
        let node = sized.node;
        let min = self.content_inline_size(sized, constraints, block_size, IntrinsicSizeCacheKind::MinContentInline);
        let max = self.content_inline_size(sized, constraints, block_size, IntrinsicSizeCacheKind::MaxContentInline);
        let sizes = ContentInlineSizes {
            sizes: ContentSizes {
                min: min.size,
                max: max.size,
            },
            first_line_max_content: max
                .first_line_max_content
                .unwrap_or(FirstLineMaxContent::without_floats(max.size)),
            dependencies: min.dependencies.union(max.dependencies),
        };
        self.charge_content_sizing_scope_with(node, sizes.dependencies);
        sizes
    }

    pub(crate) fn calculate_min_content_inline_size(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.single_content_inline_size(sized, constraints, None, IntrinsicSizeCacheKind::MinContentInline)
    }

    pub(crate) fn calculate_min_content_inline_size_at_definite_block_size(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: CssPixels,
    ) -> CssPixels {
        self.single_content_inline_size(
            sized,
            constraints,
            Some(block_size),
            IntrinsicSizeCacheKind::MinContentInline,
        )
    }

    pub(crate) fn calculate_max_content_inline_size(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.single_content_inline_size(
            sized,
            constraints,
            self.definite_block_size_of_replaced_box_in_held_metrics(sized),
            IntrinsicSizeCacheKind::MaxContentInline,
        )
    }

    pub(crate) fn calculate_max_content_inline_size_at_definite_block_size(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: CssPixels,
    ) -> CssPixels {
        self.single_content_inline_size(
            sized,
            constraints,
            Some(block_size),
            IntrinsicSizeCacheKind::MaxContentInline,
        )
    }

    fn definite_block_size_of_replaced_box_in_held_metrics(&self, sized: SizedBox<'_>) -> Option<CssPixels> {
        let node = sized.node;
        if !self.facts(node).is_replaced_box() {
            return None;
        }
        let metrics = sized.metrics_if_held()?;
        metrics.has_definite_block_size().then(|| metrics.content_block_size())
    }

    fn single_content_inline_size(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
        kind: IntrinsicSizeCacheKind,
    ) -> CssPixels {
        let node = sized.node;
        let answer = self.content_inline_size(sized, constraints, block_size, kind);
        self.charge_content_sizing_scope_with(node, answer.dependencies);
        answer.size
    }

    fn charge_content_sizing_scope_with(&self, node: Node, dependencies: IntrinsicMeasurementDependencies) {
        if self.state.is_content_sizing() {
            self.charge_measurement_dependency_to_measured_box_and_containing_block(
                node,
                dependencies.percentage_block_size,
            );
        }
    }

    fn content_inline_size(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
        kind: IntrinsicSizeCacheKind,
    ) -> ContentInlineSizeAnswer {
        let (answer, available) = match kind {
            IntrinsicSizeCacheKind::MinContentInline => (
                self.min_content_inline_size_before_transferred_limits(sized, constraints, block_size),
                AvailableSize::MinContent,
            ),
            IntrinsicSizeCacheKind::MaxContentInline => (
                self.max_content_inline_size_before_transferred_limits(sized, constraints, block_size),
                AvailableSize::MaxContent,
            ),
            _ => unreachable!("content inline size kind must use the inline axis"),
        };
        ContentInlineSizeAnswer {
            size: self.apply_transferred_limits_to_intrinsic_inline_size(sized, answer.size, available, constraints),
            ..answer
        }
    }

    fn content_inline_size_answered_from_style(
        &self,
        node: Node,
        block_size: Option<CssPixels>,
        size: CssPixels,
    ) -> ContentInlineSizeAnswer {
        let transfers_block_axis_sizes = self.facts(node).has_preferred_aspect_ratio();
        ContentInlineSizeAnswer {
            size,
            first_line_max_content: None,
            dependencies: IntrinsicMeasurementDependencies {
                percentage_block_size: transfers_block_axis_sizes
                    && self.own_style_depends_on_percentage_block_size(node),
                percentage_inline_basis: self.measurement_root_observes_percentage_inline_basis(node),
                block_size: transfers_block_axis_sizes && block_size.is_some(),
            },
        }
    }

    fn content_inline_size_ignoring_content(&self, node: Node) -> Option<ContentInlineSizeAnswer> {
        // Boxes with no children have zero intrinsic inline size.
        let size = if !self.has_children(node) && !self.has_size_without_children(node) {
            CssPixels::default()
        } else {
            self.size_contained_content_inline_size(node)?
        };
        Some(ContentInlineSizeAnswer {
            size,
            first_line_max_content: None,
            dependencies: IntrinsicMeasurementDependencies::default(),
        })
    }

    fn size_contained_content_inline_size(&self, node: Node) -> Option<CssPixels> {
        let facts = self.facts(node);
        // NB: An orthogonal box's inline axis is this context's block axis.
        (facts.node_has_size_containment()
            || (facts.node_has_inline_size_containment() && !self.establishes_orthogonal_flow(node)))
        .then(|| clamp_to_max_dimension_value(formatting_context::explicit_intrinsic_inner_inline_size(&facts)))
    }

    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
    // NB: When the block size of a box with a preferred aspect ratio is indefinite, its intrinsic inline sizes honor the
    //     min/max block sizes transferred through the ratio.
    pub(super) fn apply_transferred_limits_to_intrinsic_inline_size(
        &self,
        sized: SizedBox<'_>,
        inline_size: CssPixels,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        if !self.facts(node).has_preferred_aspect_ratio() || !self.preferred_block_size(node, constraints).is_auto() {
            return inline_size;
        }
        let available_space = AvailableSpace {
            inline_size: available,
            block_size: AvailableSize::Indefinite,
        };
        self.transferred_inline_size_limits(sized, available_space, constraints)
            .map_or(inline_size, |limits| limits.clamp(inline_size))
    }

    fn min_content_inline_size_before_transferred_limits(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> ContentInlineSizeAnswer {
        let node = sized.node;
        let facts = self.facts(node);
        let style = self.style(node);
        let answered_from_style = |size| self.content_inline_size_answered_from_style(node, block_size, size);
        // https://drafts.csswg.org/css-sizing-3/#min-content-zero
        // input with any type that is not "button-like"; this can vary depending on the UA.
        // NB: Text entry inputs are compressible even when their layout boxes are not replaced boxes.
        let is_compressible_text_input = facts.is_text_input() && style.width().contains_percentage();
        if is_compressible_text_input
            || (facts.is_replaced_box()
                && (style.width().contains_percentage() || style.max_width().contains_percentage()))
        {
            // https://www.w3.org/TR/css-sizing-3/#replaced-percentage-min-contribution
            // NOTE: If the box is replaced, a cyclic percentage in the value of any max size property or
            //       preferred size property (width/max-width/height/max-height), is resolved against zero
            //       when calculating the min-content contribution in the corresponding axis.
            // FIXME: If the box also has a preferred aspect ratio, then this min-content contribution is
            //        floored by any <length-percentage> minimum size from the opposite axis—resolving any
            //        such percentage against zero—transferred through the preferred aspect ratio.
            // Note: The min-content contribution is, as always, also floored by the minimum size in its own axis.
            let mut zero_constraints = constraints;
            zero_constraints.percentage_basis_inline_size = Some(CssPixels::default());
            let minimum = if style.min_width().is_length_percentage() {
                self.calculate_inner_inline_size(sized, AvailableSize::MinContent, style.min_width(), zero_constraints)
            } else {
                CssPixels::default()
            };
            if is_compressible_text_input {
                return answered_from_style(
                    self.calculate_inner_inline_size(sized, AvailableSize::MinContent, style.width(), zero_constraints)
                        .max(minimum),
                );
            }
            return answered_from_style(minimum);
        }
        if let Some(transferred) = self.calculate_transferred_inline_size_for_replaced_element(sized, constraints) {
            return answered_from_style(transferred);
        }
        let auto_size = self.auto_content_size(node);
        if let Some(width) = auto_size.width {
            return answered_from_style(width);
        }
        if facts.is_replaced_box()
            && !facts.has_preferred_aspect_ratio()
            && let Some(fallback) = self.max_content_size_for_replaced_element_without_natural_size(
                sized,
                auto_size,
                formatting_context::SizeDimension::Inline,
                formatting_context::ReplacedMaxContentSizeLimits::default(),
            )
        {
            return answered_from_style(fallback);
        }
        self.measure_intrinsic_inline_size(node, constraints, block_size, IntrinsicSizeCacheKind::MinContentInline)
    }

    fn max_content_inline_size_before_transferred_limits(
        &self,
        sized: SizedBox<'_>,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> ContentInlineSizeAnswer {
        let node = sized.node;
        let facts = self.facts(node);
        let style = self.style(node);
        let answered_from_style = |size| self.content_inline_size_answered_from_style(node, block_size, size);
        let mut auto_size = self.auto_content_size(node);
        if let Some(transferred) = self.calculate_transferred_inline_size_for_replaced_element(sized, constraints) {
            return answered_from_style(transferred);
        }
        if auto_size.width.is_none()
            && (facts.default_preferred_width().is_some() || facts.default_preferred_height().is_some())
        {
            // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
            // "If the box is non-replaced, then the entire value of any max size property or preferred size property
            // ('width'/'max-width'/'height'/'max-height') specified as an expression containing a percentage [...] that is
            // cyclic is treated for the purpose of calculating the box's intrinsic size contributions only as that
            // property's initial value."
            //
            // This means an `appearance: none` text input with a cyclic `width: 100%` still contributes its `width: auto`
            // size to max-content sizing. Do not use this for min-content sizing: CSS Sizing's "Compressible Replaced
            // Elements" section considers non-button-like <input> controls replaced for the percentage-sized replaced
            // element rule, so their cyclic-percentage min-content contribution can still compress toward zero.
            auto_size = formatting_context::ReplacedIntrinsicSize {
                width: facts.default_preferred_width(),
                height: facts.default_preferred_height(),
                aspect_ratio: None,
            };
        }
        if let Some(width) = auto_size.width {
            return answered_from_style(width);
        }
        if facts.is_replaced_box() {
            let definite_block_size = block_size.filter(|_| auto_size.height.is_none());
            let max_content_available = AvailableSize::MaxContent;
            let intrinsic_available_space = AvailableSpace {
                inline_size: max_content_available,
                block_size: AvailableSize::Indefinite,
            };
            let resolve_destination_inline_size = |size: &ComputedSize,
                                                   property: formatting_context::CyclicPercentageSizeProperty|
             -> Option<CssPixels> {
                if !size.is_length_percentage() {
                    return None;
                }
                match formatting_context::cyclic_percentage_intrinsic_contribution(
                    facts.is_replaced_box(),
                    size.contains_percentage(),
                    max_content_available,
                    property,
                ) {
                    formatting_context::CyclicPercentageIntrinsicContribution::TreatAsInitialValue => None,
                    formatting_context::CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                        let mut zero_constraints = constraints;
                        zero_constraints.percentage_basis_inline_size = Some(CssPixels::default());
                        Some(self.calculate_inner_inline_size(sized, max_content_available, size, zero_constraints))
                    }
                    formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => {
                        if size.contains_percentage() && constraints.percentage_basis_inline_size.is_none() {
                            None
                        } else {
                            Some(self.calculate_inner_inline_size(sized, max_content_available, size, constraints))
                        }
                    }
                }
            };
            let resolve_block_size = |size: &ComputedSize,
                                      property: formatting_context::CyclicPercentageSizeProperty|
             -> Option<CssPixels> {
                if !size.is_length_percentage() {
                    return None;
                }
                if !size.contains_percentage() || constraints.percentage_basis_block_size.is_some() {
                    return Some(self.calculate_inner_block_size(sized, intrinsic_available_space, size, constraints));
                }
                match formatting_context::cyclic_percentage_intrinsic_contribution(
                    facts.is_replaced_box(),
                    size.contains_percentage(),
                    max_content_available,
                    property,
                ) {
                    formatting_context::CyclicPercentageIntrinsicContribution::TreatAsInitialValue => None,
                    formatting_context::CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                        let mut zero_constraints = constraints;
                        zero_constraints.percentage_basis_block_size = Some(CssPixels::default());
                        Some(self.calculate_inner_block_size(sized, intrinsic_available_space, size, zero_constraints))
                    }
                    formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => None,
                }
            };

            let definite_minimum_inline_size = resolve_destination_inline_size(
                style.min_width(),
                formatting_context::CyclicPercentageSizeProperty::MinSize,
            );
            let definite_minimum_block_size = resolve_block_size(
                style.min_height(),
                formatting_context::CyclicPercentageSizeProperty::MinSize,
            );
            let replaced_constraints = formatting_context::ReplacedMaxContentSizeLimits {
                definite_size_in_ratio_determining_axis: definite_block_size,
                minimum_inline_size: definite_minimum_inline_size,
                minimum_block_size: definite_minimum_block_size,
            };
            if let Some(max_content_inline_size) = self.max_content_size_for_replaced_element_without_natural_size(
                sized,
                auto_size,
                formatting_context::SizeDimension::Inline,
                replaced_constraints,
            ) {
                if definite_block_size.is_none()
                    && facts.has_preferred_aspect_ratio()
                    && let Some(definite_maximum_block_size) = resolve_block_size(
                        style.max_height(),
                        formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
                    )
                {
                    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
                    // First, any definite minimum size is converted and transferred from the origin to destination axis.
                    // This transferred minimum is capped by any definite preferred or maximum size in the destination axis.
                    let mut transferred_minimum = definite_minimum_block_size
                        .map(|value| self.content_inline_size_from_aspect_ratio(sized, value));
                    if let Some(value) = transferred_minimum {
                        transferred_minimum = resolve_destination_inline_size(
                            style.width(),
                            formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
                        )
                        .map_or(Some(value), |resolved| Some(value.min(resolved)));
                        let value = transferred_minimum.unwrap();
                        transferred_minimum = resolve_destination_inline_size(
                            style.max_width(),
                            formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
                        )
                        .map_or(Some(value), |resolved| Some(value.min(resolved)));
                    }

                    // Then, any definite maximum size is converted and transferred from the origin to destination.
                    // This transferred maximum is floored by any definite preferred or minimum size in the destination axis
                    // as well as by the transferred minimum, if any.
                    let mut transferred_maximum =
                        self.content_inline_size_from_aspect_ratio(sized, definite_maximum_block_size);
                    if let Some(resolved) = resolve_destination_inline_size(
                        style.width(),
                        formatting_context::CyclicPercentageSizeProperty::PreferredOrMaxSize,
                    ) {
                        transferred_maximum = transferred_maximum.max(resolved);
                    }
                    if let Some(resolved) = resolve_destination_inline_size(
                        style.min_width(),
                        formatting_context::CyclicPercentageSizeProperty::MinSize,
                    ) {
                        transferred_maximum = transferred_maximum.max(resolved);
                    }
                    if let Some(transferred_minimum) = transferred_minimum {
                        transferred_maximum = transferred_maximum.max(transferred_minimum);
                    }
                    return answered_from_style(max_content_inline_size.min(transferred_maximum));
                }
                return answered_from_style(max_content_inline_size);
            }
        }
        self.measure_intrinsic_inline_size(node, constraints, block_size, IntrinsicSizeCacheKind::MaxContentInline)
    }

    // Answers from the measurement cache or from the first source that sizes `node`'s content, caching both of its
    // content inline sizes.
    fn measure_intrinsic_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
        kind: IntrinsicSizeCacheKind,
    ) -> ContentInlineSizeAnswer {
        if let Some(answer) = self.content_inline_size_ignoring_content(node) {
            return answer;
        }
        let key = formatting_context::cache_key(None, block_size, constraints);
        if let Some(cached) = self.intrinsic_inline_measurement_cache_get(node, key) {
            self.callbacks.layout_trace().intrinsic_size_hit(node, kind);
            return ContentInlineSizeAnswer::from_cache(cached, kind);
        }

        let trace = self.callbacks.layout_trace().intrinsic_size_miss(node, kind);
        let depends_on_block_size = self.intrinsic_inline_measurement_may_depend_on_block_size(node, block_size);
        let scope = || intrinsic_sizing::ContentSizingScope::new(&self.callbacks, node, constraints, block_size);
        let facts = self.facts(node);
        let (source, sizes) = if self.establishes_orthogonal_flow(node) {
            (
                "ORTHOGONAL FLOW",
                intrinsic_sizing::content_sizes_of_orthogonal_flow(self.callbacks, &scope(), constraints),
            )
        } else if facts.is_table_wrapper() {
            (
                "COLUMNS",
                intrinsic_sizing::content_sizes_of_table(self.callbacks, &scope(), constraints, depends_on_block_size),
            )
        } else {
            match formatting_context::independent_formatting_context_type(node, &self.callbacks) {
                formatting_context::FormattingContextType::Block if facts.children_are_inline() => (
                    "ITEMS",
                    intrinsic_sizing::content_sizes_of_inline_content(
                        self.callbacks,
                        &scope(),
                        constraints,
                        depends_on_block_size,
                    ),
                ),
                formatting_context::FormattingContextType::Block => (
                    "CHILDREN",
                    intrinsic_sizing::content_sizes_of_block_container(
                        self.callbacks,
                        &scope(),
                        constraints,
                        depends_on_block_size,
                    ),
                ),
                formatting_context::FormattingContextType::Table => (
                    "COLUMNS",
                    intrinsic_sizing::content_sizes_of_table(
                        self.callbacks,
                        &scope(),
                        constraints,
                        depends_on_block_size,
                    ),
                ),
                formatting_context::FormattingContextType::Flex => (
                    "FLEX ITEMS",
                    intrinsic_sizing::content_sizes_of_flex_container(
                        self.callbacks,
                        node,
                        constraints,
                        block_size,
                        depends_on_block_size,
                    ),
                ),
                formatting_context::FormattingContextType::Grid => (
                    "GRID TRACKS",
                    intrinsic_sizing::content_sizes_of_grid_container(
                        self.callbacks,
                        node,
                        constraints,
                        block_size,
                        depends_on_block_size,
                    ),
                ),
                formatting_context::FormattingContextType::Svg
                | formatting_context::FormattingContextType::InternalReplaced
                | formatting_context::FormattingContextType::ReplacedWithChildren => (
                    "REPLACED CONTENT",
                    intrinsic_sizing::content_sizes_of_replaced_content(
                        self.callbacks,
                        &scope(),
                        depends_on_block_size,
                    ),
                ),
                formatting_context::FormattingContextType::Math => (
                    "MATH LAYOUT",
                    intrinsic_sizing::content_sizes_of_math(self.callbacks, &scope(), constraints),
                ),
            }
        };
        if let Some(trace) = &trace {
            trace.append(" FROM ");
            trace.append(source);
        }
        self.cache_content_inline_sizes(node, kind, key, sizes)
    }

    fn cache_content_inline_sizes(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        sizes: ContentInlineSizes,
    ) -> ContentInlineSizeAnswer {
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            sizes.dependencies.percentage_block_size,
        );
        let measurement = IntrinsicInlineSizeMeasurement {
            min_content_inline_size: clamp_to_max_dimension_value(sizes.sizes.min),
            max_content_inline_size: clamp_to_max_dimension_value(sizes.sizes.max),
            first_line_max_content: FirstLineMaxContent {
                inline_size_of_content: clamp_to_max_dimension_value(
                    sizes.first_line_max_content.inline_size_of_content,
                ),
                inline_size_of_floats_before_first_clearance: clamp_to_max_dimension_value(
                    sizes
                        .first_line_max_content
                        .inline_size_of_floats_before_first_clearance,
                ),
                ..sizes.first_line_max_content
            },
            depends_on_percentage_block_size: sizes.dependencies.percentage_block_size,
            depends_on_percentage_inline_basis: sizes.dependencies.percentage_inline_basis,
            depends_on_block_size: sizes.dependencies.block_size,
        };
        self.intrinsic_inline_measurement_cache_put(node, key, measurement);
        ContentInlineSizeAnswer::from_cache(measurement, kind)
    }

    // Measures the max-content block size of `node`, which does not establish an orthogonal flow, at `inline_size`,
    // reusing and populating the cache.
    fn measure_intrinsic_block_size(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        debug_assert!(!self.establishes_orthogonal_flow(node));
        let kind = IntrinsicSizeCacheKind::MaxContentBlock;
        let key = formatting_context::cache_key(Some(inline_size), None, constraints);
        if let Some(cached) = self.intrinsic_block_cache_get(node, kind, key) {
            self.callbacks.layout_trace().intrinsic_size_hit(node, kind);
            return cached.size;
        }

        let result = if let Some(child_run_shapes) = self.child_run_shapes
            && let Some(shape) = child_run_shapes.child_run_shape_at(node, inline_size, constraints)
        {
            formatting_context::lay_out_shape_for_its_result(self.callbacks, shape)
        } else {
            let _trace = self.callbacks.layout_trace().intrinsic_size_miss(node, kind);
            self.measure_block_size_with_normal_layout(node, inline_size, constraints)
        };
        let value = clamp_to_max_dimension_value(result.automatic_content_block_size);
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            result.depends_on_percentage_block_size,
        );
        self.intrinsic_block_cache_put(
            node,
            kind,
            key,
            IntrinsicBlockSizeMeasurement {
                size: value,
                depends_on_percentage_block_size: result.depends_on_percentage_block_size,
                depends_on_percentage_inline_basis: self.measurement_root_observes_percentage_inline_basis(node),
            },
        );
        value
    }

    fn measure_block_size_with_normal_layout(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> formatting_context::ChildLayoutResult {
        let mut root = used_values::cells_before_sizing(&self.callbacks, node, constraints);
        root.has_definite_block_size = false;
        root.set_content_inline_size(inline_size);
        let mut input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::definite(inline_size),
                block_size: AvailableSize::Indefinite,
            },
            constraints,
            ParticipationInParentFormattingContext::Root,
        );
        input.sizing.root_is_measured_for_its_content_block_size = matches!(
            formatting_context::independent_formatting_context_type(node, &self.callbacks),
            formatting_context::FormattingContextType::Flex | formatting_context::FormattingContextType::Grid
        );
        formatting_context::lay_out_measurement_root_for_its_result(self.callbacks, node, root, input)
    }

    /// The min-content or max-content block size of a box that establishes an orthogonal flow: its block axis here is
    /// its inline axis in its own frame, so this is its content inline size of `kind` there, at `inline_size` as its
    /// block size.
    fn content_block_size_of_orthogonal_flow(
        &self,
        sized: SizedBox<'_>,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
        kind: IntrinsicSizeCacheKind,
    ) -> CssPixels {
        let node = sized.node;
        let own_frame = formatting_context::frame_established_by(&self.callbacks, node);
        let sizing = SizingContext::new(self.state, self.callbacks.in_frame_of(own_frame.writing_mode()));
        let constraints_in_own_frame = ContainingBlockConstraints {
            percentage_basis_inline_size: constraints.percentage_basis_block_size,
            percentage_basis_block_size: constraints.percentage_basis_inline_size,
            quirks_mode_percentage_basis_block_size: None,
        };
        // NB: The box's own metrics are read in the box's own frame. Cells its parent sizes it in are read as they are.
        let own_metrics_in_own_frame = sized
            .own_metrics_frame
            .map(|held_in| sized.metrics().cells().in_frame(held_in, sizing.callbacks.frame));
        let sized_in_own_frame = match &own_metrics_in_own_frame {
            Some(cells) => SizedBox::of_own_metrics(node, cells, sizing.callbacks.frame),
            None => sized,
        };
        let answer = sizing.content_inline_size(sized_in_own_frame, constraints_in_own_frame, Some(inline_size), kind);
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            answer.dependencies.percentage_block_size,
        );
        answer.size
    }

    pub(crate) fn calculate_min_content_block_size(
        &self,
        sized: SizedBox<'_>,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // https://www.w3.org/TR/css-sizing-3/#min-content-block-size
        let facts = self.facts(node);
        // NB: The block axis of a box that establishes an orthogonal flow is its own inline axis.
        if self.establishes_orthogonal_flow(node) && self.has_children(node) {
            return self.content_block_size_of_orthogonal_flow(
                sized,
                inline_size,
                constraints,
                IntrinsicSizeCacheKind::MinContentInline,
            );
        }
        if facts.is_replaced_box() {
            let auto_size = self.auto_content_size(node);
            if let Some(height) = auto_size.height {
                return auto_size.aspect_ratio.map_or(height, |ratio| ratio.divide(inline_size));
            }
        }
        // For block containers, tables, and inline boxes, this is equivalent to the max-content block size.
        self.calculate_max_content_block_size(sized, inline_size, constraints)
    }

    pub(crate) fn calculate_max_content_block_size(
        &self,
        sized: SizedBox<'_>,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        if self.establishes_orthogonal_flow(node) && self.has_children(node) {
            return self.content_block_size_of_orthogonal_flow(
                sized,
                inline_size,
                constraints,
                IntrinsicSizeCacheKind::MaxContentInline,
            );
        }
        if self.facts(node).has_preferred_aspect_ratio() {
            // OPTIMIZATION: The automatic minimum size measures the content, so the result is cached.
            let kind = IntrinsicSizeCacheKind::MaxContentBlock;
            let key = formatting_context::cache_key(Some(inline_size), None, constraints);
            if let Some(cached) = self.intrinsic_block_cache_get(node, kind, key) {
                return cached.size;
            }
            let block_size = self.content_block_size_from_aspect_ratio(sized, inline_size);
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
            let minimum = self.automatic_minimum_size_from_aspect_ratio(
                sized,
                SizingAxis::Block,
                inline_size,
                block_size,
                AvailableSpace {
                    inline_size: AvailableSize::definite(inline_size),
                    block_size: AvailableSize::Indefinite,
                },
                constraints,
                None,
            );
            let size = minimum.map_or(block_size, |minimum| block_size.max(minimum));
            self.intrinsic_block_cache_put(
                node,
                kind,
                key,
                IntrinsicBlockSizeMeasurement {
                    size,
                    depends_on_percentage_block_size: self.own_style_depends_on_percentage_block_size(node),
                    depends_on_percentage_inline_basis: self.measurement_root_observes_percentage_inline_basis(node),
                },
            );
            return size;
        }
        let auto_size = self.auto_content_size(node);
        if let Some(height) = auto_size.height {
            return height;
        }
        if let Some(fallback) = self.max_content_size_for_replaced_element_without_natural_size(
            sized,
            auto_size,
            formatting_context::SizeDimension::Block,
            formatting_context::ReplacedMaxContentSizeLimits::default(),
        ) {
            return fallback;
        }
        // Boxes with no children have zero intrinsic height.
        if !self.has_children(node) && !self.has_size_without_children(node) {
            return CssPixels::default();
        }
        if self.establishes_orthogonal_flow(node) {
            return self.content_block_size_of_orthogonal_flow(
                sized,
                inline_size,
                constraints,
                IntrinsicSizeCacheKind::MaxContentInline,
            );
        }
        self.measure_intrinsic_block_size(node, inline_size, constraints)
    }

    pub(crate) fn measure_automatic_content_block_size(
        &self,
        sized: SizedBox<'_>,
        inner_available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // https://drafts.csswg.org/css-flexbox-1/#algo-main-container
        // The automatic block size of a flex container is its max-content size.
        if self.style(node).display().is_flex_inside() {
            return self.calculate_max_content_block_size(
                sized,
                inner_available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }
        formatting_context::lay_out_measurement_root_for_its_result(
            self.callbacks,
            node,
            used_values::cells_before_sizing(&self.callbacks, node, constraints),
            LayoutInput::new(
                inner_available_space,
                constraints,
                ParticipationInParentFormattingContext::Root,
            ),
        )
        .automatic_content_block_size
    }

    /// Buttons and text inputs center their content within their content box, which therefore has to be definite
    /// before the content is laid out, also where a min or max block size decides an automatic block size.
    pub(crate) fn make_form_control_content_box_definite(
        &self,
        sized_cells: SizedCells<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        measured_content_block_size: Option<CssPixels>,
    ) {
        let sized = sized_cells.sized_box();
        let node = sized.node;
        let facts = self.facts(node);
        // https://drafts.csswg.org/css-ui-4/#input-rules
        // The content is vertically centered
        // NB: A text input that lays out its content as an ordinary box centers its content too, including content
        //     that overflows a content box made shorter by a max-height.
        let text_input_content_is_ordinary_box = facts.lays_out_text_input_content_as_ordinary_box();
        if !facts.uses_button_layout() && !text_input_content_is_ordinary_box {
            return;
        }
        // Flex/grid-inside controls are their own flex/grid container, which aligns their content itself, so there is
        // nothing to make definite for centering.
        let style = self.style(node);
        if style.display().is_flex_inside() || style.display().is_grid_inside() {
            return;
        }
        // With auto height and no min-height the content box already exactly wraps the content, so there is
        // no extra space to center within and no need to force a definite content box. A text input's max-height can
        // still make its content box shorter than its content.
        let max_height_can_clamp_text_input_content =
            text_input_content_is_ordinary_box && !style.max_height().is_auto();
        if style.height().is_auto() && is_plain_auto(style.min_height()) && !max_height_can_clamp_text_input_content {
            return;
        }
        if sized.metrics().has_definite_block_size() {
            return;
        }
        let natural = measured_content_block_size.unwrap_or_else(|| {
            self.measure_automatic_content_block_size(
                sized,
                sized
                    .metrics()
                    .available_inner_space_or_constraints_from(available_space),
                constraints,
            )
        });
        let block_size_is_automatic = self.should_treat_block_size_as_auto(sized, available_space, constraints);
        let mut used_block_size = if block_size_is_automatic {
            natural
        } else {
            self.calculate_inner_block_size(
                sized,
                available_space,
                self.preferred_block_size(node, constraints),
                constraints,
            )
        };
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            used_block_size = used_block_size.min(self.calculate_inner_block_size(
                sized,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !is_plain_auto(style.min_height()) {
            used_block_size = used_block_size.max(self.calculate_inner_block_size(
                sized,
                available_space,
                style.min_height(),
                constraints,
            ));
        }
        // Only force a definite content box when a button's used block size exceeds its content block size, so a larger
        // preferred or minimum size has room to center within, or when a text input's differs from it in either
        // direction. A content-sized box stays indefinite, so an intrinsic keyword does not resolve percentage-sized
        // descendants.
        let content_box_needs_to_be_definite = if text_input_content_is_ordinary_box {
            used_block_size != natural
        } else {
            used_block_size > natural
        };
        if !content_box_needs_to_be_definite {
            return;
        }
        sized_cells.cells.update_cells(|cells| {
            cells.set_content_block_size(used_block_size);
            cells.has_definite_block_size = true;
            cells.has_definite_block_size_only_for_button_content_alignment =
                block_size_is_automatic && facts.uses_button_layout();
        });
    }

    pub(crate) fn table_box_inside_wrapper(&self, wrapper: Node) -> Node {
        fn find(context: &SizingContext, parent: Node) -> Option<Node> {
            context.callbacks.children(parent).find_map(|child| {
                let facts = context.facts(child);
                if facts.is_box() && facts.display().is_table_inside() {
                    return Some(child);
                }
                find(context, child)
            })
        }

        find(self, wrapper).expect("table wrapper must contain a table box")
    }

    fn size_table_box_inside_wrapper<R>(
        &self,
        wrapper: Node,
        containing_block_inline_size: CssPixels,
        table_constraints: ContainingBlockConstraints,
        size_table: impl FnOnce(&FormattingContextRun<'_>, used_values::RootInputCells) -> R,
    ) -> R {
        let table_box = self.table_box_inside_wrapper(wrapper);
        let mut table_cells = used_values::cells_before_sizing(&self.callbacks, table_box, table_constraints);
        let table_style = self.style(table_box);
        table_cells.border_left = table_style.border_left_width();
        table_cells.border_right = table_style.border_right_width();
        table_cells.padding_left = table_style.padding_left().to_px(containing_block_inline_size);
        table_cells.padding_right = table_style.padding_right().to_px(containing_block_inline_size);

        RunState::with_content_sizing_root(
            self.callbacks,
            table_box,
            self.callbacks.in_flow_containing_block(table_box),
            |state, callbacks| {
                let table_run = FormattingContextRun {
                    state,
                    box_: table_box,
                    callbacks,
                    should_collect_devtools_layout_data: false,
                    treat_block_axis_percentage_insets_as_auto_beyond_root: false,
                    fragments: None,
                    fragmentainer: None,
                };
                size_table(&table_run, table_cells)
            },
        )
    }

    // 17.5.2 Table width algorithms: the 'table-layout' property
    // https://www.w3.org/TR/CSS22/tables.html#width-layout
    /// Returns the inline size of the wrapper and the inline layout of the table box it found, which the run of the
    /// table box can reuse.
    pub(crate) fn compute_table_box_inline_size_inside_wrapper(
        &self,
        wrapper: Node,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
        table_wrapper_containing_block_inline_size: Option<CssPixels>,
        table_wrapper_inline_size_mode: formatting_context::TableWrapperInlineSizeMode,
    ) -> (CssPixels, table_formatting_context::TableInlineLayout) {
        // CSS 2 says the table wrapper inline size is the border-edge inline size of the table grid box inside it.

        let style = self.style(wrapper);
        let containing_block_inline_size =
            table_wrapper_containing_block_inline_size.unwrap_or_else(|| available_space.inline_size.to_px_or_zero());

        // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
        let margin_left = style.margin_left().to_px(containing_block_inline_size);
        let margin_right = style.margin_right().to_px(containing_block_inline_size);

        // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
        let available_inline_size = containing_block_inline_size - margin_left - margin_right;

        // The table wrapper is invisible to percentage resolution, so the table box gets the
        // wrapper's constraints unchanged. Callers measuring a table wrapper for grid alignment
        // pass the grid-area inline size as the wrapper's percentage basis.
        let table_constraints = table_wrapper_constraints;
        self.size_table_box_inside_wrapper(
            wrapper,
            containing_block_inline_size,
            table_constraints,
            |table_run, table_cells| {
                let mut table = table_formatting_context::TableFormattingContext::new(
                    table_run,
                    table_cells,
                    table_formatting_context::AutoTableInlineSize::ShrinksToFitAvailableInlineSize,
                );
                let table_available = table_cells.available_inner_space_or_constraints_from(available_space);
                table.run_until_inline_size_calculation(
                    LayoutInput::new(
                        table_available,
                        table_constraints,
                        ParticipationInParentFormattingContext::Root,
                    ),
                    true,
                );

                let table_used = table.table_box_cells();
                let table_used_inline_size =
                    table_used.border_box_inline_size(table_used.uses_collapsing_borders_model());
                let inline_size = if table_wrapper_inline_size_mode
                    == formatting_context::TableWrapperInlineSizeMode::UseTableUsedInlineSizeIfNotAuto
                    && !self.style(table_run.box_).width().is_auto()
                {
                    table_used_inline_size
                } else if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                    table_used_inline_size.min(available_inline_size)
                } else {
                    table_used_inline_size
                };
                (inline_size, table.take_inline_layout())
            },
        )
    }

    pub(crate) fn table_wrapper_content_inline_sizes(
        &self,
        wrapper: Node,
        available_block_size: AvailableSize,
        table_wrapper_constraints: ContainingBlockConstraints,
    ) -> ContentSizes {
        let indefinite_containing_block_inline_size = CssPixels::default();
        let (table_box, table_border_box_inline_sizes, table_depends_on_percentage_block_size) = self
            .size_table_box_inside_wrapper(
                wrapper,
                indefinite_containing_block_inline_size,
                table_wrapper_constraints,
                |table_run, table_cells| {
                    let sizes = table_formatting_context::TableFormattingContext::new(
                        table_run,
                        table_cells,
                        table_formatting_context::AutoTableInlineSize::ShrinksToFitAvailableInlineSize,
                    )
                    .border_box_inline_sizes(LayoutInput::new(
                        AvailableSpace {
                            inline_size: AvailableSize::MaxContent,
                            block_size: available_block_size,
                        },
                        table_wrapper_constraints,
                        ParticipationInParentFormattingContext::Root,
                    ));
                    (
                        table_run.box_,
                        sizes,
                        table_run
                            .sizing()
                            .resolve_percentage_block_size_dependency(table_run.box_),
                    )
                },
            );
        formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
            self.state,
            &self.callbacks,
            table_box,
            table_depends_on_percentage_block_size,
        );
        table_border_box_inline_sizes
    }

    // 17.5.3 Table height algorithms
    // https://www.w3.org/TR/CSS22/tables.html#height-layout
    pub(crate) fn compute_table_box_block_size_inside_wrapper(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.measure_table_box_block_size_inside_wrapper(sized, available_space, table_wrapper_constraints)
            .table_box_border_box_block_size
    }

    pub(crate) fn measure_table_box_block_size_inside_wrapper(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
    ) -> TableWrapperBlockSizes {
        let wrapper = sized.node;
        // The table wrapper block size should equal the block size of the table box it contains.

        let style = self.style(wrapper);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let containing_block_block_size = available_space.block_size.to_px_or_zero();

        // If 'margin-top', or 'margin-bottom' are computed as 'auto', their used value is '0'.
        let margin_top = style.margin_top().to_px(containing_block_inline_size);
        let margin_bottom = style.margin_bottom().to_px(containing_block_inline_size);

        // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
        let available_block_size = containing_block_block_size - margin_top - margin_bottom;

        let wrapper_inner_space = sized
            .metrics()
            .available_inner_space_or_constraints_from(available_space);
        let mut wrapper_cells = used_values::cells_before_sizing(&self.callbacks, wrapper, table_wrapper_constraints);
        if let AvailableSize::Definite(inline_size) = wrapper_inner_space.inline_size {
            wrapper_cells.set_content_inline_size(inline_size);
        }
        if let AvailableSize::Definite(block_size) = wrapper_inner_space.block_size {
            wrapper_cells.set_content_block_size(block_size);
        } else {
            wrapper_cells.has_definite_block_size = false;
        }
        let wrapper_outputs = formatting_context::lay_out_measurement_root_for_its_result(
            self.callbacks,
            wrapper,
            wrapper_cells,
            LayoutInput::new(
                wrapper_inner_space,
                table_wrapper_constraints,
                ParticipationInParentFormattingContext::Root,
            ),
        );

        let table_used_block_size = wrapper_outputs
            .table_box_in_wrapper_border_box_block_size
            .expect("a table wrapper's measurement run lays out the table box inside it");
        let table_box_border_box_block_size = if matches!(available_space.block_size, AvailableSize::Definite(_)) {
            table_used_block_size.min(available_block_size)
        } else {
            table_used_block_size
        };
        TableWrapperBlockSizes {
            table_box_border_box_block_size,
            wrapper_content_block_size: wrapper_outputs.automatic_content_block_size,
        }
    }

    pub(crate) fn calculate_fit_content_size(
        &self,
        sized: SizedBox<'_>,
        axis: SizingAxis,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#fit-content-size
        match axis {
            SizingAxis::Inline => {
                // If the available space in a given axis is definite, equal to clamp(min-content size, stretch-fit size,
                // max-content size) (i.e. max(min-content size, min(max-content size, stretch-fit size))).
                if let AvailableSize::Definite(basis) = available_space.inline_size {
                    let stretch = self.stretch_fit_inner_size(sized, SizingAxis::Inline, basis, constraints);
                    let max_content = self.calculate_max_content_inline_size(sized, constraints);
                    if max_content <= stretch {
                        return max_content;
                    }
                    return self.calculate_min_content_inline_size(sized, constraints).max(stretch);
                }
                // When sizing under a min-content constraint, equal to the min-content size.
                if available_space.inline_size == AvailableSize::MinContent {
                    return self.calculate_min_content_inline_size(sized, constraints);
                }
                // Otherwise, equal to the max-content size in that axis.
                self.calculate_max_content_inline_size(sized, constraints)
            }
            SizingAxis::Block => {
                let inline_size = available_space.inline_size.to_px_or_zero();
                // https://drafts.csswg.org/css-sizing-3/#fit-content-size
                // If the available space in a given axis is definite,
                // equal to clamp(min-content size, stretch-fit size, max-content size)
                // (i.e. max(min-content size, min(max-content size, stretch-fit size))).
                if let AvailableSize::Definite(basis) = available_space.block_size {
                    let stretch = self.stretch_fit_inner_size(sized, SizingAxis::Block, basis, constraints);
                    let max_content = self.calculate_max_content_block_size(sized, inline_size, constraints);
                    if max_content <= stretch {
                        return max_content;
                    }
                    return self
                        .calculate_min_content_block_size(sized, inline_size, constraints)
                        .max(stretch);
                }
                // Otherwise, equal to the max-content size in that axis.
                self.calculate_max_content_block_size(sized, inline_size, constraints)
            }
        }
    }

    /// The used content size of a box whose size property is `size`, given the content size its basis resolved
    /// to. A calc-size() resolves its calculation against that size; any other size keeps it.
    /// https://drafts.csswg.org/css-values-5/#resolving-calc-size
    pub(crate) fn resolve_calc_size(
        &self,
        sized: SizedBox<'_>,
        axis: SizingAxis,
        size: &ComputedSize,
        content_size: CssPixels,
        percentage_basis: Option<CssPixels>,
    ) -> CssPixels {
        let node = sized.node;
        let Some(calc_size) = size.calc_size() else {
            return content_size;
        };
        // https://drafts.csswg.org/css-values-5/#valdef-calc-size-calc-sum
        // For all purposes other than actually resolving the size, the calc-size() acts like this calculation wrapped
        // in a calc() (including, for example, whether it represents a definite or indefinite size).
        // NB: A size given as a calculation reaches here when it behaves as auto, so the calc-size() does too.
        if crate::css::calc_size::calc_size_has_calc_sum_basis(calc_size) {
            return content_size;
        }
        // The size is that of the box the box-sizing property names.
        let style = self.style(node);
        let edges = if style.box_sizing() == box_sizing::BORDER_BOX {
            let metrics = sized.metrics();
            match axis {
                SizingAxis::Inline => {
                    style.border_left_width()
                        + metrics.padding_left()
                        + metrics.padding_right()
                        + style.border_right_width()
                }
                SizingAxis::Block => {
                    style.border_top_width()
                        + metrics.padding_top()
                        + metrics.padding_bottom()
                        + style.border_bottom_width()
                }
            }
        } else {
            CssPixels::default()
        };
        let basis_size = (content_size + edges).to_double();
        let Some(resolved) =
            crate::css::calc_size::resolve_calc_size(calc_size, percentage_basis.map(CssPixels::to_double), &|_| {
                basis_size
            })
        else {
            return content_size;
        };
        (CssPixels::nearest_value_for(resolved) - edges).max(CssPixels::default())
    }

    /// The inner size that stretch-fit sizing gives a box whose containing block has the size `basis` in `axis`.
    /// https://drafts.csswg.org/css-sizing-3/#stretch-fit-sizing
    pub(crate) fn stretch_fit_inner_size(
        &self,
        sized: SizedBox<'_>,
        axis: SizingAxis,
        basis: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style(node);
        let metrics = sized.metrics();
        let margin_basis = constraints.inline_basis();
        // Sizes the margin box to fill the containing block exactly, treating auto margins as zero.
        // (If this would make the inner size negative, it instead sizes the margin box so that the inner size is zero.)
        let (margins, edges) = match axis {
            SizingAxis::Inline => (
                style.margin_left().to_px(margin_basis) + style.margin_right().to_px(margin_basis),
                style.border_left_width()
                    + metrics.padding_left()
                    + metrics.padding_right()
                    + style.border_right_width(),
            ),
            SizingAxis::Block => {
                let (margin_top, margin_bottom) = stretch_fit_block_margins(&self.callbacks, node, margin_basis);
                (
                    margin_top + margin_bottom,
                    style.border_top_width()
                        + metrics.padding_top()
                        + metrics.padding_bottom()
                        + style.border_bottom_width(),
                )
            }
        };
        (basis - margins - edges).max(CssPixels::default())
    }

    /// The used inline size of a box that establishes an orthogonal flow and whose inline size its containing block
    /// leaves undetermined, in the box's own frame.
    pub(crate) fn orthogonal_flow_inline_size(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style(node);
        let initial_containing_block_size = if self.callbacks.frame.is_transposed() {
            self.callbacks.initial_containing_block_block_size
        } else {
            self.callbacks.initial_containing_block_inline_size
        };
        // https://drafts.csswg.org/css-writing-modes-4/#orthogonal-auto
        // In these cases, an additional constraint is used as a fallback in place of the available inline space for
        // calculations that require a definite available inline space-- the smallest of
        // - the size represented by the containing block’s inner max size (if that is fixed) floored by its inner min
        //   size (if that is fixed)
        // - the nearest ancestor scrollport’s inner size if that is fixed, else / capped by its inner max size if
        //   that is fixed, floored by its inner min size if that is fixed
        // - the initial containing block’s size
        let fixed_size = |size: &ComputedSize| {
            (size.is_length_percentage() && !size.contains_percentage()).then(|| size.to_px(CssPixels::default()))
        };
        let containing_block = self.callbacks.in_flow_containing_block(node);
        let mut scrollport_size = None;
        let mut ancestor = containing_block;
        while !ancestor.is_invalid() {
            if self.facts(ancestor).is_scroll_container() {
                let style = self.style(ancestor);
                let inner = |size: CssPixels| {
                    if style.box_sizing() == box_sizing::BORDER_BOX {
                        (size
                            - style.border_left_width()
                            - style.border_right_width()
                            - style.padding_left().to_px(CssPixels::default())
                            - style.padding_right().to_px(CssPixels::default()))
                        .max(CssPixels::default())
                    } else {
                        size
                    }
                };
                let maximum = fixed_size(style.max_width()).map(inner);
                let minimum = fixed_size(style.min_width()).map(inner);
                scrollport_size = fixed_size(style.width())
                    .map(inner)
                    .map(|size| maximum.map_or(size, |maximum| size.min(maximum)))
                    .or(maximum)
                    .map(|size| minimum.map_or(size, |minimum| size.max(minimum)));
                break;
            }
            ancestor = self.callbacks.parent(ancestor);
        }
        let scrollport_and_initial_containing_block_size = scrollport_size
            .map_or(initial_containing_block_size, |size| {
                size.min(initial_containing_block_size)
            });
        // NB: An intrinsic size measurement of the box has no available space, but its containing block's size, which
        //     the available space is when it is definite, still arrives as the percentage basis.
        let available_inline_size = match (available_space.inline_size, constraints.percentage_basis_inline_size) {
            (AvailableSize::Definite(size), _) | (_, Some(size)) => size,
            _ => {
                let containing_block_max_size = (!containing_block.is_invalid())
                    .then(|| {
                        let style = self.style(containing_block);
                        fixed_size(style.max_width()).map(|maximum| {
                            fixed_size(style.min_width()).map_or(maximum, |minimum| maximum.max(minimum))
                        })
                    })
                    .flatten();
                containing_block_max_size.map_or(scrollport_and_initial_containing_block_size, |maximum| {
                    maximum.min(scrollport_and_initial_containing_block_size)
                })
            }
        };
        let inline_space = AvailableSpace {
            inline_size: AvailableSize::definite(available_inline_size),
            block_size: available_space.block_size,
        };
        let mut inline_size = if self.should_treat_inline_size_as_auto(sized, inline_space) {
            // https://drafts.csswg.org/css-writing-modes-4/#auto-multicol
            // If 'column-count' and 'column-width' are both auto, use the shrink-to-fit formula
            // min(max-content, max(min-content, constraint)), where:
            // min-content: the min-content inline size of the box
            // max-content: the max-content inline size of the box
            // constraint: the inline-axis size that would stretch fit into to the smallest of
            // - the available space: the containing block’s size if that is fixed, else the size represented by the
            //   containing block’s inner max size (if that is fixed) floored by its inner min size (if that is fixed)
            // - the nearest ancestor scrollport’s inner size if that is fixed, else / capped by its inner max size if
            //   that is fixed, floored by its inner min size if that is fixed
            // - the initial containing block’s size
            // FIXME: Lay out the box in columns of that width where its content overflows its block size.
            let constraint = self.calculate_stretch_fit_inline_size(
                sized,
                AvailableSize::definite(available_inline_size.min(scrollport_and_initial_containing_block_size)),
            );
            let max_content = self.calculate_max_content_inline_size(sized, constraints);
            if max_content <= constraint {
                max_content
            } else {
                self.calculate_min_content_inline_size(sized, constraints)
                    .max(constraint)
            }
        } else {
            self.calculate_inner_inline_size(sized, inline_space.inline_size, style.width(), constraints)
        };
        if !self.should_treat_max_inline_size_as_none(node, inline_space.inline_size, constraints) {
            inline_size = inline_size.min(self.calculate_inner_inline_size(
                sized,
                inline_space.inline_size,
                style.max_width(),
                constraints,
            ));
        }
        if !is_plain_auto(style.min_width()) {
            inline_size = inline_size.max(self.calculate_inner_inline_size(
                sized,
                inline_space.inline_size,
                style.min_width(),
                constraints,
            ));
        }
        inline_size.max(CssPixels::default())
    }

    pub(crate) fn calculate_inner_inline_size(
        &self,
        sized: SizedBox<'_>,
        available: AvailableSize,
        preferred_size: &ComputedSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        // NB: Outside flex and grid layout, the automatic minimum size is zero, which a minimum given as a calc-size()
        //     with an auto basis resolves against.
        if preferred_size.is_auto() {
            assert!(preferred_size.calc_size().is_some());
            return self.resolve_calc_size(
                sized,
                SizingAxis::Inline,
                preferred_size,
                CssPixels::default(),
                constraints.percentage_basis_inline_size,
            );
        }
        let basis = if preferred_size.contains_percentage() {
            if let Some(basis) = constraints.percentage_basis_inline_size {
                basis
            } else {
                available.to_px_or_zero()
            }
        } else {
            available.to_px_or_zero()
        };
        if preferred_size.is_stretch() {
            let stretch_fit_size = self.stretch_fit_inner_size(sized, SizingAxis::Inline, basis, constraints);
            return self.resolve_calc_size(
                sized,
                SizingAxis::Inline,
                preferred_size,
                stretch_fit_size,
                constraints.percentage_basis_inline_size,
            );
        }
        let keyword_size = if preferred_size.is_fit_content() {
            Some(self.calculate_fit_content_size(
                sized,
                SizingAxis::Inline,
                AvailableSpace {
                    inline_size: available,
                    block_size: AvailableSize::Indefinite,
                },
                constraints,
            ))
        } else if preferred_size.is_max_content() {
            Some(self.calculate_max_content_inline_size(sized, constraints))
        } else if preferred_size.is_min_content() {
            Some(self.calculate_min_content_inline_size(sized, constraints))
        } else {
            None
        };
        if let Some(keyword_size) = keyword_size {
            return self.resolve_calc_size(
                sized,
                SizingAxis::Inline,
                preferred_size,
                keyword_size,
                constraints.percentage_basis_inline_size,
            );
        }
        let value = preferred_size.to_px(basis);
        let style = self.style(node);
        if style.box_sizing() == box_sizing::BORDER_BOX {
            let metrics = sized.metrics();
            return formatting_context::subtract_border_box_adjustment(
                value,
                style.border_left_width(),
                metrics.padding_left(),
                style.border_right_width(),
                metrics.padding_right(),
            );
        }
        value
    }

    pub(crate) fn calculate_inner_block_size(
        &self,
        sized: SizedBox<'_>,
        available_space: AvailableSpace,
        preferred_size: &ComputedSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        if preferred_size.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            // NB: A replaced element's calc-size() height resolves against its own automatic height, which its width
            //     then follows through the ratio.
            if preferred_size.calc_size().is_some() && self.facts(node).is_replaced_box() {
                return self.compute_block_size_for_replaced_element(sized, available_space, constraints);
            }
            let block_size = self.content_block_size_from_aspect_ratio(sized, sized.metrics().content_inline_size());
            return self.resolve_calc_size(
                sized,
                SizingAxis::Block,
                preferred_size,
                block_size,
                constraints.percentage_basis_block_size,
            );
        }
        // NB: Outside flex and grid layout, the automatic minimum size is zero, which a minimum given as a calc-size()
        //     with an auto basis resolves against.
        if preferred_size.is_auto() {
            assert!(preferred_size.calc_size().is_some());
            return self.resolve_calc_size(
                sized,
                SizingAxis::Block,
                preferred_size,
                CssPixels::default(),
                constraints.percentage_basis_block_size,
            );
        }
        let facts = self.facts(node);
        let keyword_size = if facts.is_replaced_box()
            && facts.has_auto_content_box_size()
            && preferred_size.is_intrinsic_sizing_constraint()
        {
            // NB: The intrinsic block sizes of a replaced element are its natural height, or the height that follows
            //     its used width through its aspect ratio.
            let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
            match intrinsic.height {
                Some(height) if is_plain_auto(self.style(node).width()) => Some(height),
                _ if facts.has_preferred_aspect_ratio() => {
                    Some(self.content_block_size_from_aspect_ratio(sized, sized.metrics().content_inline_size()))
                }
                height => Some(height.unwrap_or_else(|| self.default_replaced_size(SizingAxis::Block))),
            }
        } else if preferred_size.is_fit_content() {
            Some(self.calculate_fit_content_size(sized, SizingAxis::Block, available_space, constraints))
        } else if preferred_size.is_max_content() {
            Some(self.calculate_max_content_block_size(sized, available_space.inline_size.to_px_or_zero(), constraints))
        } else if preferred_size.is_min_content() {
            Some(self.calculate_min_content_block_size(sized, available_space.inline_size.to_px_or_zero(), constraints))
        } else {
            None
        };
        if let Some(keyword_size) = keyword_size {
            return self.resolve_calc_size(
                sized,
                SizingAxis::Block,
                preferred_size,
                keyword_size,
                constraints.percentage_basis_block_size,
            );
        }

        let mut basis = available_space.block_size.to_px_or_zero();
        // NOTE: Percentage heights are resolved against the containing block's used height,
        //       not the available space height. The containing block's height must be definite
        //       for percentage resolution to work (otherwise should_treat_block_size_as_auto
        //       should have returned true and we wouldn't be here).
        // NOTE: We only do this when available space height is indefinite. If it's definite,
        //       we trust that the caller has set it up correctly (e.g., grid/flex items get
        //       their cell/area size as available space).
        if preferred_size.contains_percentage() && available_space.block_size == AvailableSize::Indefinite {
            // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
            // NOTE: Flex/grid items resolve percentage heights against their container, not via quirk.
            let facts = self.facts(node);
            if facts.document_in_quirks_mode()
                && !facts.is_anonymous()
                && !facts.parent_is_flex_or_grid_container()
                && !facts.is_in_user_agent_shadow_tree()
            {
                basis = constraints.quirks_mode_percentage_basis_block_size.unwrap_or_default();
            } else {
                basis = constraints.block_basis();
            }
        }
        if preferred_size.is_stretch() {
            let stretch_fit_size = self.stretch_fit_inner_size(sized, SizingAxis::Block, basis, constraints);
            return self.resolve_calc_size(
                sized,
                SizingAxis::Block,
                preferred_size,
                stretch_fit_size,
                constraints.percentage_basis_block_size,
            );
        }
        let value = preferred_size.to_px(basis);
        let style = self.style(node);
        if style.box_sizing() == box_sizing::BORDER_BOX {
            let metrics = sized.metrics();
            return formatting_context::subtract_border_box_adjustment(
                value,
                style.border_top_width(),
                metrics.padding_top(),
                style.border_bottom_width(),
                metrics.padding_bottom(),
            );
        }
        value
    }

    pub(crate) fn calculate_inner_size_for_property(
        &self,
        sized: SizedBox<'_>,
        axis: SizingAxis,
        property: SizingProperty,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let node = sized.node;
        let style = self.style(node);
        let size = match property {
            SizingProperty::Width => style.width(),
            SizingProperty::Height => style.height(),
            SizingProperty::MinWidth => style.min_width(),
            SizingProperty::MinHeight => style.min_height(),
            SizingProperty::MaxWidth => style.max_width(),
            SizingProperty::MaxHeight => style.max_height(),
            SizingProperty::FlexBasis => style.flex_basis(),
        };
        match axis {
            SizingAxis::Inline => {
                self.calculate_inner_inline_size(sized, available_space.inline_size, size, constraints)
            }
            SizingAxis::Block => self.calculate_inner_block_size(sized, available_space, size, constraints),
        }
    }

    pub(crate) fn should_treat_size_as_auto(
        &self,
        sized: SizedBox<'_>,
        axis: SizingAxis,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        match axis {
            SizingAxis::Inline => self.should_treat_inline_size_as_auto(sized, available_space),
            SizingAxis::Block => self.should_treat_block_size_as_auto(sized, available_space, constraints),
        }
    }
}

#[cfg(test)]
mod replaced_ratio_tests {
    use super::*;
    use crate::css::computed_values::{
        BoxValues, GroupPayloadRef, SizingValues, clone_group_payload, release_group_payload,
    };
    use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
    use crate::css::table_group_builder::group_index;
    use crate::layout::formatting_context::LayoutStageFacts;
    use crate::layout::layout_node_arena::LayoutNodeArena;
    use crate::layout::node_data::{NodeConstructionFacts, NodeKind, StylePayloadsRef};
    use crate::layout::run_state::LayoutScratch;

    fn size(
        width: Option<i64>,
        height: Option<i64>,
        ratio: Option<(i64, i64)>,
        fallback: bool,
        contained: bool,
    ) -> (i32, i32) {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let mut style = FfiStylePayloads {
            groups: std::array::from_fn(|index| GroupPayloadRef::default_payload(index).as_ptr()),
        };
        // SAFETY: Each cloned immortal default payload is private until installed in this test's arena.
        let box_payload = unsafe { clone_group_payload(group_index::BOX, style.groups[group_index::BOX]) };
        let sizing_payload = unsafe { clone_group_payload(group_index::SIZING, style.groups[group_index::SIZING]) };
        // SAFETY: These clones have the corresponding payload layouts and no readers yet.
        unsafe {
            let box_values = &mut *box_payload.cast::<BoxValues>();
            box_values.display = crate::css::display::FfiDisplay::block();
            box_values.aspect_ratio.has_preferred_ratio = fallback;
            box_values.aspect_ratio.preferred_ratio_numerator = 2.0;
            box_values.aspect_ratio.preferred_ratio_denominator = 1.0;
            box_values.size_containment = contained;
            let length = RetainedStyleValueData::from_owned(StyleValueData::Length {
                value: 40.0,
                unit: crate::css::style_compute::px_length_unit(),
            });
            (*sizing_payload.cast::<SizingValues>()).max_height = ComputedSize::from_data(&length);
        }
        style.groups[group_index::BOX] = box_payload;
        style.groups[group_index::SIZING] = sizing_payload;
        let node = arena.allocate(NodeConstructionFacts {
            kind: NodeKind::ImageBox,
            is_anonymous: false,
            is_html_input_element: false,
            is_html_html_element: false,
            is_document_element: false,
            is_in_user_agent_shadow_tree: false,
            uses_button_layout: false,
            is_editing_host: false,
            is_body: false,
            dom_paint_facts: 0,
            style_node: 0,
        });
        arena.set_node_style(node, 0, StylePayloadsRef::new(&raw const style));
        arena.set_replaced_content_facts(
            node,
            node_facts::ReplacedContentFacts {
                auto_content_size: formatting_context::ReplacedIntrinsicSize {
                    width: width.map(CssPixels::from_integer),
                    height: height.map(CssPixels::from_integer),
                    aspect_ratio: ratio.map(|(numerator, denominator)| formatting_context::PixelFraction {
                        numerator: CssPixels::from_integer(numerator),
                        denominator: CssPixels::from_integer(denominator),
                    }),
                },
                ..Default::default()
            },
        );
        let callbacks = LayoutPass::new(
            &arena,
            &scratch,
            LayoutStageFacts {
                viewport_inline_size: CssPixels::from_integer(800),
                viewport_block_size: CssPixels::from_integer(600),
                document_in_quirks_mode: false,
            },
        );
        let result = RunState::with_root(callbacks, node, Node::INVALID, None, |state, callbacks| {
            let context = SizingContext::new(state, callbacks);
            let mut metrics = used_values::RootInputCells::default();
            let available = AvailableSpace {
                inline_size: AvailableSize::Definite(CssPixels::from_integer(800)),
                block_size: AvailableSize::Indefinite,
            };
            let constraints = ContainingBlockConstraints::default();
            let inline =
                context.compute_inline_size_for_replaced_element(SizedBox::new(node, &metrics), available, constraints);
            // NB: Match dimension_atomic_root: publish the used inline size before resolving the block size.
            metrics.set_content_inline_size(inline);
            let block =
                context.compute_block_size_for_replaced_element(SizedBox::new(node, &metrics), available, constraints);
            (inline.to_int(), block.to_int())
        });
        arena
            .free_subtree(node)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        release_group_payload(group_index::SIZING, sizing_payload);
        release_group_payload(group_index::BOX, box_payload);
        result
    }

    #[test]
    fn auto_ratio_width_only_is_constrained_through_the_fallback_ratio() {
        assert_eq!(size(Some(100), None, None, true, false), (80, 40));
    }

    #[test]
    fn auto_ratio_height_only_is_constrained_through_the_fallback_ratio() {
        assert_eq!(size(None, Some(100), None, true, false), (80, 40));
    }

    #[test]
    fn auto_ratio_preserves_the_natural_ratio() {
        assert_eq!(size(Some(100), Some(100), Some((1, 1)), true, false), (40, 40));
    }

    #[test]
    fn plain_auto_without_a_ratio_keeps_independent_dimensions() {
        assert_eq!(size(Some(100), Some(20), None, false, false), (100, 20));
    }

    #[test]
    fn size_containment_uses_the_fallback_ratio_with_explicit_inner_sizes() {
        assert_eq!(size(Some(100), Some(20), None, true, true), (80, 40));
    }
}
