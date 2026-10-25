/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) struct ContentSizingScope {
    node: Node,
    root: UsedValues,
}

impl ContentSizingScope {
    pub(super) fn new(
        callbacks: &LayoutPass<'_>,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> Self {
        let root = used_values::create_used_values(callbacks, node, constraints);
        // NB: A parent layout can assign a definite block size that is not present in computed style,
        //     such as the stretched cross size of a flex item. Preserve it so descendant percentages
        //     resolve against the same size during intrinsic measurement.
        if let Some(block_size) = block_size {
            root.set_content_block_size(block_size);
            root.has_definite_block_size.set(true);
        }
        root.has_definite_inline_size.set(false);
        Self { node, root }
    }

    pub(super) fn root(&self) -> &UsedValues {
        &self.root
    }

    pub(super) fn available_block_size(&self) -> AvailableSize {
        if self.root.has_definite_block_size() {
            AvailableSize::definite(self.root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        }
    }

    fn enter<R>(&self, callbacks: LayoutPass<'_>, size_content: impl FnOnce(&FormattingContextRun<'_>) -> R) -> R {
        RunRecords::with_content_inline_sizes_root(
            callbacks,
            self.node,
            callbacks.in_flow_containing_block(self.node),
            &self.root,
            |records, callbacks| {
                size_content(&FormattingContextRun {
                    records,
                    box_: self.node,
                    callbacks,
                    should_collect_devtools_layout_data: false,
                    treat_block_axis_percentage_insets_as_auto_beyond_root: false,
                    fragments: None,
                    previous_line_data: None,
                    fragmentainer: None,
                })
            },
        )
    }
}

// NB: A box that establishes an orthogonal flow contributes its block size to the inline sizes measured in the frame
//     of `callbacks`, which only its layout finds.
fn establishes_orthogonal_flow(callbacks: &LayoutPass<'_>, node: Node) -> bool {
    formatting_context::frame_established_by(callbacks, node).is_orthogonal_to(callbacks.frame)
}

pub(super) fn content_sizes_of_inline_content(
    callbacks: LayoutPass<'_>,
    scope: &ContentSizingScope,
    constraints: ContainingBlockConstraints,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
    let node = scope.node;
    let facts = NodeFacts::new(&callbacks, node);
    let style = StyleValues::for_node(&callbacks, node);
    // NB: These contexts can couple inline size to block layout or have special content sizing rules.
    if !facts.children_are_inline()
        || formatting_context::independent_formatting_context_type(node, &callbacks)
            != formatting_context::FormattingContextType::Block
        || facts.is_table_wrapper()
        || style.writing_mode() != writing_mode::HORIZONTAL_TB
        || establishes_orthogonal_flow(&callbacks, node)
    {
        return None;
    }
    scope.enter(callbacks, |run| {
        let child_constraints = run.sizing().constraints_for_child_context(node, constraints);
        let sizes = if inline_content_has_no_in_flow_content(&callbacks, node) {
            let sizing = run.sizing();
            let mut contributions_of_floats = BlockLevelContributions::default();
            contributions_of_floats.place_floats_among_children_of(
                &callbacks,
                &sizing,
                node,
                scope.available_block_size(),
                child_constraints,
            );
            sizing_context::ContentInlineSizes {
                sizes: contributions_of_floats.largest,
                first_line_max_content: FirstLineMaxContent::without_floats(contributions_of_floats.largest.max),
                dependencies: IntrinsicMeasurementDependencies {
                    percentage_block_size: sizing.resolve_percentage_block_size_dependency(node),
                    percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(node),
                    block_size: depends_on_block_size,
                },
            }
        } else {
            let input = LayoutInput::new(
                AvailableSpace {
                    inline_size: AvailableSize::MaxContent,
                    block_size: scope.available_block_size(),
                },
                child_constraints,
                ParticipationInParentFormattingContext::BlockLevel,
            );
            let parent = block_formatting_context::BlockFormattingContext::new(run);
            let mut context = inline_formatting_context::InlineFormattingContext::new_with_rust_parent(
                run,
                run.box_,
                input,
                run.callbacks,
                &parent,
            );
            let iterator = inline_level_iterator::InlineLevelIterator::for_intrinsic_inline_size(&mut context);
            let sizes = context.content_sizes_from_items(iterator.items(), depends_on_block_size);
            iterator.stash_for_reuse(&context);
            sizes
        };
        if !facts.lays_out_in_columns() {
            return Some(sizes);
        }
        let multicol_sizes = multicol_content_sizes(style, sizes.sizes);
        Some(sizing_context::ContentInlineSizes {
            sizes: multicol_sizes,
            first_line_max_content: FirstLineMaxContent::without_floats(multicol_sizes.max),
            ..sizes
        })
    })
}

fn multicol_content_sizes(
    style: StyleValues<'_>,
    largest_contributions: sizing_context::ContentSizes,
) -> sizing_context::ContentSizes {
    sizing_context::ContentSizes {
        min: fragmentation::multicol_intrinsic_inline_size(style, largest_contributions.min, true),
        max: fragmentation::multicol_intrinsic_inline_size(style, largest_contributions.max, false),
    }
}

fn child_is_inline_level(callbacks: &LayoutPass<'_>, child: Node) -> bool {
    let facts = NodeFacts::new(callbacks, child);
    facts.is_flow_layout_participant() && facts.is_inline()
}

fn child_is_inline_level_box_that_block_flow_cannot_lay_out(callbacks: &LayoutPass<'_>, child: Node) -> bool {
    child_is_inline_level(callbacks, child)
        && formatting_context::formatting_context_type_created_by_box(NodeFacts::new(callbacks, child)).is_none()
}

pub(super) fn floats_to_the_left(style: StyleValues<'_>) -> bool {
    matches!(style.float_(), float::LEFT | float::INLINE_START)
}

pub(super) fn clears_left_floats(clear: u8) -> bool {
    matches!(clear, clear::LEFT | clear::BOTH | clear::INLINE_START)
}

pub(super) fn clears_right_floats(clear: u8) -> bool {
    matches!(clear, clear::RIGHT | clear::BOTH | clear::INLINE_END)
}

#[derive(Clone, Copy, Default)]
struct FloatsBesideNextBlockLevelBox {
    left_inline_size: CssPixels,
    right_inline_size: CssPixels,
}

impl FloatsBesideNextBlockLevelBox {
    fn inline_size(self) -> CssPixels {
        self.left_inline_size + self.right_inline_size
    }

    fn is_empty(self) -> bool {
        self.inline_size() == CssPixels::default()
    }

    fn clear(&mut self, clear: u8) {
        if clears_left_floats(clear) {
            self.left_inline_size = CssPixels::default();
        }
        if clears_right_floats(clear) {
            self.right_inline_size = CssPixels::default();
        }
    }

    fn add(&mut self, floats_to_the_left: bool, margin_box_inline_size: CssPixels) {
        if margin_box_inline_size <= CssPixels::default() {
            return;
        }
        if floats_to_the_left {
            self.left_inline_size += margin_box_inline_size;
        } else {
            self.right_inline_size += margin_box_inline_size;
        }
    }

    fn max_content_of_first_line_beside_them(self, first_line: FirstLineMaxContent) -> CssPixels {
        let mut floats_not_cleared_by_first_line = self;
        floats_not_cleared_by_first_line.clear(first_line.clear_of_first_float);
        first_line.inline_size_of_content
            + self.inline_size().max(
                floats_not_cleared_by_first_line.inline_size()
                    + first_line.inline_size_of_floats_before_first_clearance,
            )
    }

    fn line_insets_of_box_avoiding_them(self, margin_left: CssPixels, margin_right: CssPixels) -> CssPixels {
        let inset = |floats_inline_size: CssPixels, margin: CssPixels| {
            if margin > CssPixels::default() {
                floats_inline_size.max(margin)
            } else {
                floats_inline_size + margin
            }
        };
        inset(self.left_inline_size, margin_left) + inset(self.right_inline_size, margin_right)
    }
}

#[derive(Default)]
struct BlockLevelContributions {
    largest: sizing_context::ContentSizes,
    floats_beside_next_box: FloatsBesideNextBlockLevelBox,
}

impl BlockLevelContributions {
    fn clear_floats(&mut self, clear: u8) {
        if clear == clear::NONE {
            return;
        }
        self.largest.max = self.largest.max.max(self.floats_beside_next_box.inline_size());
        self.floats_beside_next_box.clear(clear);
    }

    fn place_float(&mut self, float_style: StyleValues<'_>, margin_box_contributions: sizing_context::ContentSizes) {
        self.clear_floats(float_style.clear());
        self.floats_beside_next_box
            .add(floats_to_the_left(float_style), margin_box_contributions.max);
        self.largest = self.largest.largest_of(sizing_context::ContentSizes {
            min: margin_box_contributions.min,
            max: self.floats_beside_next_box.inline_size(),
        });
    }

    fn place_floats_among_children_of(
        &mut self,
        callbacks: &LayoutPass<'_>,
        sizing: &sizing_context::SizingContext<'_, '_>,
        container: Node,
        available_block_size: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) {
        for float in callbacks
            .children(container)
            .filter(|&child| NodeFacts::new(callbacks, child).is_floating())
        {
            let margin_box_contributions = sizing
                .outer_inline_contributions(float, available_block_size, constraints)
                .margin_box_contributions();
            self.place_float(StyleValues::for_node(callbacks, float), margin_box_contributions);
        }
    }

    fn add_in_flow_box(&mut self, contributions: sizing_context::ContentSizes) {
        self.largest = self.largest.largest_of(contributions);
        self.floats_beside_next_box = FloatsBesideNextBlockLevelBox::default();
    }
}

fn is_anonymous_wrapper_of_inline_content(facts: NodeFacts<'_>) -> bool {
    facts.is_anonymous()
        && !facts.is_generated_for_pseudo_element()
        && facts.is_block_container()
        && facts.children_are_inline()
        && !facts.is_table_wrapper()
}

fn text_is_collapsible_whitespace(callbacks: &LayoutPass<'_>, text_node: Node) -> bool {
    StyleValues::for_node(callbacks, callbacks.parent(text_node)).white_space_collapse()
        == white_space_collapse::COLLAPSE
        && !NodeFacts::new(callbacks, text_node).produces_line_box_fragment_when_empty()
        && callbacks
            .text_content(text_node)
            .text
            .iter()
            .all(|unit| matches!(unit, 0x09..=0x0d | 0x20))
}

fn inline_content_has_no_in_flow_content(callbacks: &LayoutPass<'_>, block_container: Node) -> bool {
    callbacks.children(block_container).all(|child| {
        let facts = NodeFacts::new(callbacks, child);
        if facts.is_text_node() {
            text_is_collapsible_whitespace(callbacks, child)
        } else {
            facts.is_box() && (facts.is_floating() || facts.is_absolutely_positioned())
        }
    })
}

fn is_anonymous_wrapper_without_in_flow_content(callbacks: &LayoutPass<'_>, node: Node) -> bool {
    is_anonymous_wrapper_of_inline_content(NodeFacts::new(callbacks, node))
        && inline_content_has_no_in_flow_content(callbacks, node)
}

fn rendered_legend_with_automatic_inline_size(callbacks: &LayoutPass<'_>, fieldset: Node) -> Option<Node> {
    let legend = NodeFacts::new(callbacks, fieldset).rendered_legend();
    (!legend.is_invalid() && StyleValues::for_node(callbacks, legend).width().is_auto()).then_some(legend)
}

pub(super) fn content_sizes_of_block_container(
    callbacks: LayoutPass<'_>,
    scope: &ContentSizingScope,
    constraints: ContainingBlockConstraints,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
    let node = scope.node;
    let facts = NodeFacts::new(&callbacks, node);
    let style = StyleValues::for_node(&callbacks, node);
    if facts.children_are_inline()
        || formatting_context::independent_formatting_context_type(node, &callbacks)
            != formatting_context::FormattingContextType::Block
        || facts.is_table_wrapper()
        || (style.writing_mode() != writing_mode::HORIZONTAL_TB
            && block_formatting_context::is_line_clamp_container(&callbacks, node))
        || establishes_orthogonal_flow(&callbacks, node)
        || callbacks
            .children(node)
            .any(|child| child_is_inline_level_box_that_block_flow_cannot_lay_out(&callbacks, child))
    {
        return None;
    }
    Some(scope.enter(callbacks, |run| {
        let sizing = run.sizing();
        let child_constraints = sizing.constraints_for_child_context(node, constraints);
        let collects_column_spanners = facts.lays_out_in_columns();
        let legend_sized_to_fit_its_content = rendered_legend_with_automatic_inline_size(&callbacks, node);
        let mut contributions_of_children = BlockLevelContributions::default();
        let mut largest_column_spanner_contributions = sizing_context::ContentSizes::default();
        for child in callbacks.children(node) {
            let child_facts = NodeFacts::new(&callbacks, child);
            if !child_facts.is_flow_layout_participant() {
                continue;
            }
            let child_avoids_floats =
                block_formatting_context::box_avoids_floats_because_it_establishes_formatting_context(
                    &callbacks, child,
                );
            if !child_avoids_floats && is_anonymous_wrapper_without_in_flow_content(&callbacks, child) {
                contributions_of_children.place_floats_among_children_of(
                    &callbacks,
                    &sizing,
                    child,
                    scope.available_block_size(),
                    child_constraints,
                );
                continue;
            }
            let contributions = if child_facts.is_table_wrapper() {
                sizing.block_level_table_wrapper_outer_inline_contributions(child, child_constraints)
            } else if legend_sized_to_fit_its_content == Some(child) {
                sizing.outer_inline_contributions_of_legend_whose_fit_content_size_ignores_its_limits(
                    child,
                    scope.available_block_size(),
                    child_constraints,
                )
            } else {
                sizing.outer_inline_contributions(child, scope.available_block_size(), child_constraints)
            };
            let margin_box_contributions = contributions.margin_box_contributions();
            if collects_column_spanners && block_formatting_context::is_column_spanner(&callbacks, child) {
                largest_column_spanner_contributions =
                    largest_column_spanner_contributions.largest_of(margin_box_contributions);
                continue;
            }
            let child_style = StyleValues::for_node(&callbacks, child);
            if child_facts.is_floating() {
                contributions_of_children.place_float(child_style, margin_box_contributions);
                continue;
            }
            if child_avoids_floats {
                contributions_of_children.clear_floats(child_style.clear());
            }
            let floats = contributions_of_children.floats_beside_next_box;
            let max_content_contribution = if child_avoids_floats {
                let border_box_max_content_inline_size = contributions.border_start
                    + contributions.padding_start
                    + contributions.max_content_inline_size
                    + contributions.padding_end
                    + contributions.border_end;
                border_box_max_content_inline_size
                    + floats.line_insets_of_box_avoiding_them(contributions.margin_start, contributions.margin_end)
            } else if !floats.is_empty() && is_anonymous_wrapper_of_inline_content(child_facts) {
                let first_line_max_content = sizing
                    .content_inline_sizes(child, child_constraints, None)
                    .first_line_max_content;
                margin_box_contributions
                    .max
                    .max(floats.max_content_of_first_line_beside_them(first_line_max_content))
            } else {
                margin_box_contributions.max
            };
            contributions_of_children.add_in_flow_box(sizing_context::ContentSizes {
                min: margin_box_contributions.min,
                max: max_content_contribution,
            });
        }
        let sizes = if collects_column_spanners {
            multicol_content_sizes(style, contributions_of_children.largest)
                .largest_of(largest_column_spanner_contributions)
        } else {
            contributions_of_children.largest
        };
        sizing_context::ContentInlineSizes {
            sizes,
            first_line_max_content: FirstLineMaxContent::without_floats(sizes.max),
            dependencies: IntrinsicMeasurementDependencies {
                percentage_block_size: sizing.resolve_percentage_block_size_dependency(node),
                percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(node),
                block_size: depends_on_block_size,
            },
        }
    }))
}

pub(super) fn content_sizes_of_table(
    callbacks: LayoutPass<'_>,
    scope: &ContentSizingScope,
    constraints: ContainingBlockConstraints,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
    let node = scope.node;
    let facts = NodeFacts::new(&callbacks, node);
    let is_table_box = formatting_context::independent_formatting_context_type(node, &callbacks)
        == formatting_context::FormattingContextType::Table;
    if !(facts.is_table_wrapper() || is_table_box)
        || StyleValues::for_node(&callbacks, node).writing_mode() != writing_mode::HORIZONTAL_TB
        || establishes_orthogonal_flow(&callbacks, node)
    {
        return None;
    }
    Some(scope.enter(callbacks, |run| {
        let sizing = run.sizing();
        let sizes = if is_table_box {
            table_formatting_context::TableFormattingContext::new(
                run,
                table_formatting_context::AutoTableInlineSize::FillsAvailableInlineSize,
            )
            .content_inline_sizes(LayoutInput::new(
                AvailableSpace {
                    inline_size: AvailableSize::MaxContent,
                    block_size: scope.available_block_size(),
                },
                constraints,
                ParticipationInParentFormattingContext::Root,
            ))
        } else {
            sizing.table_wrapper_content_inline_sizes(node, scope.available_block_size(), constraints)
        };
        sizing_context::ContentInlineSizes {
            sizes,
            first_line_max_content: FirstLineMaxContent::without_floats(sizes.max),
            dependencies: IntrinsicMeasurementDependencies {
                percentage_block_size: sizing.resolve_percentage_block_size_dependency(node),
                percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(node),
                block_size: depends_on_block_size,
            },
        }
    }))
}

pub(super) fn content_sizes_of_flex_container(
    callbacks: LayoutPass<'_>,
    node: Node,
    constraints: ContainingBlockConstraints,
    block_size: Option<CssPixels>,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
    if !container_content_is_sized_without_layout(&callbacks, node, formatting_context::FormattingContextType::Flex) {
        return None;
    }
    Some(content_sizes_of_container_under_each_inline_size_constraint(
        callbacks,
        node,
        constraints,
        block_size,
        depends_on_block_size,
        None,
        |run, input| flex_formatting_context::FlexFormattingContext::new(run).content_inline_size(input),
    ))
}

pub(super) fn content_sizes_of_grid_container(
    callbacks: LayoutPass<'_>,
    node: Node,
    constraints: ContainingBlockConstraints,
    block_size: Option<CssPixels>,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
    if !container_content_is_sized_without_layout(&callbacks, node, formatting_context::FormattingContextType::Grid) {
        return None;
    }
    Some(content_sizes_of_container_under_each_inline_size_constraint(
        callbacks,
        node,
        constraints,
        block_size,
        depends_on_block_size,
        content_size_named_by_maximum_inline_size(&callbacks, node),
        |run, input| grid_formatting_context::GridFormattingContext::new(run, None).content_inline_size(input),
    ))
}

fn container_content_is_sized_without_layout(
    callbacks: &LayoutPass<'_>,
    node: Node,
    formatting_context_type: formatting_context::FormattingContextType,
) -> bool {
    formatting_context::independent_formatting_context_type(node, callbacks) == formatting_context_type
        && StyleValues::for_node(callbacks, node).writing_mode() == writing_mode::HORIZONTAL_TB
        && !establishes_orthogonal_flow(callbacks, node)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum ContentSizeNamedByMaximum {
    MinContent,
    MaxContent,
}

impl ContentSizeNamedByMaximum {
    pub(super) fn available_inline_size(self) -> AvailableSize {
        match self {
            Self::MinContent => AvailableSize::MinContent,
            Self::MaxContent => AvailableSize::MaxContent,
        }
    }
}

pub(super) fn content_size_named_by_maximum_inline_size(
    callbacks: &LayoutPass<'_>,
    node: Node,
) -> Option<ContentSizeNamedByMaximum> {
    let maximum = StyleValues::for_node(callbacks, node).max_width();
    if maximum.calc_size().is_some() {
        None
    } else if maximum.is_min_content() {
        Some(ContentSizeNamedByMaximum::MinContent)
    } else if maximum.is_max_content() {
        Some(ContentSizeNamedByMaximum::MaxContent)
    } else {
        None
    }
}

pub(super) fn available_inline_size_under_content_size_named_by_maximum(
    content_size_named_by_maximum_inline_size: Option<ContentSizeNamedByMaximum>,
) -> AvailableSize {
    content_size_named_by_maximum_inline_size.map_or(
        AvailableSize::MaxContent,
        ContentSizeNamedByMaximum::available_inline_size,
    )
}

fn content_sizes_of_container_under_each_inline_size_constraint(
    callbacks: LayoutPass<'_>,
    node: Node,
    constraints: ContainingBlockConstraints,
    block_size: Option<CssPixels>,
    depends_on_block_size: bool,
    content_size_named_by_maximum_inline_size: Option<ContentSizeNamedByMaximum>,
    content_inline_size: impl Fn(&FormattingContextRun<'_>, LayoutInput) -> CssPixels,
) -> sizing_context::ContentInlineSizes {
    let content_inline_size_under =
        |available_inline_size: AvailableSize, content_size_named_by_maximum: Option<CssPixels>| {
            let scope = ContentSizingScope::new(&callbacks, node, constraints, block_size);
            scope.enter(callbacks, |run| {
                let sizing = run.sizing();
                let mut input = LayoutInput::new(
                    AvailableSpace {
                        inline_size: available_inline_size,
                        block_size: scope.available_block_size(),
                    },
                    constraints,
                    ParticipationInParentFormattingContext::Root,
                );
                input.sizing.maximum_inline_size_resolved_by_caller =
                    content_size_named_by_maximum.map(|content_size| {
                        sizing.apply_transferred_limits_to_intrinsic_inline_size(
                            node,
                            clamp_to_max_dimension_value(content_size),
                            available_inline_size_under_content_size_named_by_maximum(
                                content_size_named_by_maximum_inline_size,
                            ),
                            constraints,
                        )
                    });
                let size = content_inline_size(run, input);
                let dependencies = IntrinsicMeasurementDependencies {
                    percentage_block_size: sizing.resolve_percentage_block_size_dependency(node),
                    percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(node),
                    block_size: depends_on_block_size,
                };
                (size, dependencies)
            })
        };
    let ((min, min_content_dependencies), (max, max_content_dependencies)) =
        measure_under_each_inline_size_constraint_named_maximum_first(
            content_size_named_by_maximum_inline_size,
            content_inline_size_under,
            |(size, _)| *size,
        );
    sizing_context::ContentInlineSizes {
        sizes: sizing_context::ContentSizes { min, max },
        first_line_max_content: FirstLineMaxContent::without_floats(max),
        dependencies: min_content_dependencies.union(max_content_dependencies),
    }
}

pub(super) fn measure_under_each_inline_size_constraint_named_maximum_first<Measurement>(
    content_size_named_by_maximum_inline_size: Option<ContentSizeNamedByMaximum>,
    measure_under: impl Fn(AvailableSize, Option<CssPixels>) -> Measurement,
    content_inline_size_of: impl Fn(&Measurement) -> CssPixels,
) -> (Measurement, Measurement) {
    match content_size_named_by_maximum_inline_size {
        Some(ContentSizeNamedByMaximum::MinContent) => {
            let min_content = measure_under(AvailableSize::MinContent, None);
            let max_content = measure_under(AvailableSize::MaxContent, Some(content_inline_size_of(&min_content)));
            (min_content, max_content)
        }
        Some(ContentSizeNamedByMaximum::MaxContent) => {
            let max_content = measure_under(AvailableSize::MaxContent, None);
            let min_content = measure_under(AvailableSize::MinContent, Some(content_inline_size_of(&max_content)));
            (min_content, max_content)
        }
        None => (
            measure_under(AvailableSize::MinContent, None),
            measure_under(AvailableSize::MaxContent, None),
        ),
    }
}

pub(super) fn content_sizes_of_replaced_content(
    callbacks: LayoutPass<'_>,
    scope: &ContentSizingScope,
    depends_on_block_size: bool,
) -> Option<sizing_context::ContentInlineSizes> {
    let node = scope.node;
    if establishes_orthogonal_flow(&callbacks, node) {
        return None;
    }
    let size = match formatting_context::independent_formatting_context_type(node, &callbacks) {
        formatting_context::FormattingContextType::Svg
        | formatting_context::FormattingContextType::InternalReplaced
        | formatting_context::FormattingContextType::InternalDummy => CssPixels::default(),
        formatting_context::FormattingContextType::ReplacedWithChildren => scope.root().content_inline_size.get(),
        _ => return None,
    };
    Some(scope.enter(callbacks, |run| {
        let sizing = run.sizing();
        sizing_context::ContentInlineSizes {
            sizes: sizing_context::ContentSizes { min: size, max: size },
            first_line_max_content: FirstLineMaxContent::without_floats(size),
            dependencies: IntrinsicMeasurementDependencies {
                percentage_block_size: sizing.resolve_percentage_block_size_dependency(node),
                percentage_inline_basis: sizing.measurement_root_observes_percentage_inline_basis(node),
                block_size: depends_on_block_size,
            },
        }
    }))
}
