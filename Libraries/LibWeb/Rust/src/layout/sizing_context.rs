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

pub(crate) struct SizingContext {
    purpose: formatting_context::LayoutPurpose,
    records: std::rc::Rc<RunRecords>,
    callbacks: FfiLayoutFcCallbacks,
}

impl SizingContext {
    pub(crate) fn new(
        purpose: formatting_context::LayoutPurpose,
        records: std::rc::Rc<RunRecords>,
        callbacks: FfiLayoutFcCallbacks,
    ) -> Self {
        Self {
            purpose,
            records,
            callbacks,
        }
    }

    pub(super) fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    pub(super) fn style(&self, node: Node) -> StyleValues<'static> {
        StyleValues::for_node(&self.callbacks, node)
    }

    #[track_caller]
    fn used(&self, node: Node) -> std::rc::Rc<UsedValues> {
        self.records.used_values(node)
    }

    fn parent(&self, node: Node) -> Node {
        self.callbacks.parent(node)
    }

    fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.next_sibling(node)
    }

    fn has_children(&self, node: Node) -> bool {
        !self.callbacks.first_child(node).is_invalid()
    }

    fn block_size_for_intrinsic_inline_measurement_cache_key(
        &self,
        root: Node,
        block_size: Option<CssPixels>,
    ) -> Option<CssPixels> {
        let block_size = block_size?;
        // OPTIMIZATION: A definite block size only distinguishes intrinsic inline measurements when a cross-axis
        // dependency can transfer it back into the inline axis. Reuse the common horizontal-flow measurement across
        // assigned block sizes, while preserving distinct entries for orthogonal flows and aspect-ratio transfers.
        let depends_on_block_size =
            self.callbacks
                .arena()
                .intrinsic_inline_size_depends_on_block_size(self.callbacks.node_data(root), || {
                    let mut pending = vec![root];
                    while let Some(node) = pending.pop() {
                        let facts = self.facts(node);
                        if !facts.is_text_node() {
                            let style = self.style(node);
                            if style.writing_mode() != writing_mode::HORIZONTAL_TB {
                                return true;
                            }
                            if style.display().is_flex_inside()
                                && matches!(
                                    style.flex_direction(),
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
                        let mut child = self.first_child(node);
                        while !child.is_invalid() {
                            pending.push(child);
                            child = self.next_sibling(child);
                        }
                    }
                    false
                });
        depends_on_block_size.then_some(block_size)
    }

    fn content_block_size_from_aspect_ratio(&self, node: Node, content_inline_size: CssPixels) -> CssPixels {
        let style = self.style(node);
        let used = self.used(node);
        formatting_context::content_block_size_from_aspect_ratio_values(
            content_inline_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + used.padding_left.get(),
            style.border_right_width() + used.padding_right.get(),
            style.border_top_width() + used.padding_top.get(),
            style.border_bottom_width() + used.padding_bottom.get(),
        )
    }

    fn content_inline_size_from_aspect_ratio(&self, node: Node, content_block_size: CssPixels) -> CssPixels {
        let style = self.style(node);
        let used = self.used(node);
        formatting_context::content_inline_size_from_aspect_ratio_values(
            content_block_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + used.padding_left.get(),
            style.border_right_width() + used.padding_right.get(),
            style.border_top_width() + used.padding_top.get(),
            style.border_bottom_width() + used.padding_bottom.get(),
        )
    }

    fn auto_content_size(&self, node: Node) -> formatting_context::ReplacedIntrinsicSize {
        let facts = self.facts(node);
        formatting_context::ReplacedIntrinsicSize {
            width: facts.has_auto_content_width().then_some(facts.auto_content_width()),
            height: facts.has_auto_content_height().then_some(facts.auto_content_height()),
            aspect_ratio: facts
                .has_auto_content_aspect_ratio()
                .then_some(formatting_context::PixelFraction {
                    numerator: facts.auto_content_aspect_ratio_numerator(),
                    denominator: facts.auto_content_aspect_ratio_denominator(),
                }),
        }
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
            width: facts
                .has_default_preferred_width()
                .then_some(facts.default_preferred_width()),
            height: facts
                .has_default_preferred_height()
                .then_some(facts.default_preferred_height()),
            aspect_ratio: None,
        }
    }

    fn max_content_size_for_replaced_element_without_natural_size(
        &self,
        node: Node,
        natural_size: formatting_context::ReplacedIntrinsicSize,
        dimension: formatting_context::SizeDimension,
        constraints: formatting_context::ReplacedMaxContentSizeConstraints,
    ) -> Option<CssPixels> {
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
                    self.content_inline_size_from_aspect_ratio(node, size)
                } else {
                    self.content_block_size_from_aspect_ratio(node, size)
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
                    self.content_block_size_from_aspect_ratio(node, inline_size)
                })
            } else if style.min_width().is_length_percentage() && !style.min_width().contains_percentage() {
                let inline_size = style.min_width().to_px(CssPixels::default());
                Some(if is_inline_axis {
                    inline_size
                } else {
                    self.content_block_size_from_aspect_ratio(node, inline_size)
                })
            } else {
                None
            };
            let size_from_min_block = if let Some(block_size) = constraints.minimum_block_size {
                Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(node, block_size)
                } else {
                    block_size
                })
            } else if style.min_height().is_length_percentage() && !style.min_height().contains_percentage() {
                let block_size = style.min_height().to_px(CssPixels::default());
                Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(node, block_size)
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
                    self.content_block_size_from_aspect_ratio(node, facts.initial_containing_block_inline_size())
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
        Some(CssPixels::from_integer(if is_inline_axis { 300 } else { 150 }))
    }

    fn tentative_inline_size_for_replaced_element(
        &self,
        node: Node,
        computed_inline_size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.3.2 Inline, replaced elements, https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
        // Treat percentages of indefinite containing block widths as 0 (the initial width).
        if computed_inline_size.is_percentage() && constraints.percentage_basis_inline_size.is_none() {
            return CssPixels::default();
        }
        let style = self.style(node);
        let computed_block_size = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            auto_computed_size()
        } else {
            style.height()
        };
        let used_inline_size = if computed_inline_size.is_auto() {
            computed_inline_size.to_px(available_space.inline_size.to_px_or_zero())
        } else {
            self.calculate_inner_inline_size(node, available_space.inline_size, computed_inline_size, constraints)
        };
        // If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic width,
        // then that intrinsic width is the used value of 'width'.
        let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
        if computed_block_size.is_auto()
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
        if (computed_block_size.is_auto()
            && computed_inline_size.is_auto()
            && intrinsic.width.is_none()
            && intrinsic.height.is_some()
            && has_ratio)
            || (computed_inline_size.is_auto() && !computed_block_size.is_auto() && has_ratio)
        {
            let block_size = self.compute_block_size_for_replaced_element(node, available_space, constraints);
            return self.content_inline_size_from_aspect_ratio(node, block_size);
        }
        // If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but no intrinsic height or width,
        // then the used value of 'width' is undefined in CSS 2.2. However, it is suggested that, if the containing block's width does not itself
        // depend on the replaced element's width, then the used value of 'width' is calculated from the constraint equation used for block-level,
        // non-replaced elements in normal flow.
        if computed_block_size.is_auto()
            && computed_inline_size.is_auto()
            && intrinsic.width.is_none()
            && intrinsic.height.is_none()
            && has_ratio
        {
            if !available_space.inline_size.is_intrinsic_sizing_constraint() {
                return self.calculate_stretch_fit_inline_size(node, available_space.inline_size);
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
                    return self.calculate_stretch_fit_inline_size(node, available_space.inline_size);
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
            return CssPixels::from_integer(300);
        }
        used_inline_size
    }

    fn tentative_block_size_for_replaced_element(
        &self,
        node: Node,
        computed_block_size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
        // https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-height
        let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
        // If 'height' and 'width' both have computed values of 'auto' and the element also has
        // an intrinsic height, then that intrinsic height is the used value of 'height'.
        if self.should_treat_inline_size_as_auto(node, available_space)
            && self.should_treat_block_size_as_auto(node, available_space, constraints)
            && let Some(height) = intrinsic.height
        {
            return height;
        }
        // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the used value of 'height' is:
        //
        //     (used width) / (intrinsic ratio)
        if computed_block_size.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            return self.content_block_size_from_aspect_ratio(node, self.used(node).content_inline_size.get());
        }
        // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then that intrinsic height is the used value of 'height'.
        //
        // Otherwise, if 'height' has a computed value of 'auto', but none of the conditions above are met,
        // then the used value of 'height' must be set to the height of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px,
        // and has a width not greater than the device width.
        if computed_block_size.is_auto() {
            return intrinsic.height.unwrap_or_else(|| CssPixels::from_integer(150));
        }
        // FIXME: Handle cases when available_space is not definite.
        self.calculate_inner_block_size(node, available_space, computed_block_size, constraints)
    }

    fn solve_replaced_size_constraint(
        &self,
        node: Node,
        input_inline_size: CssPixels,
        input_block_size: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> (CssPixels, CssPixels) {
        // 10.4 Minimum and maximum widths: 'min-width' and 'max-width'
        // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
        let style = self.style(node);
        let min_inline = if style.min_width().is_auto() {
            CssPixels::default()
        } else {
            self.calculate_inner_inline_size(node, available_space.inline_size, style.min_width(), constraints)
        };
        let specified_max_inline =
            if self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
                input_inline_size
            } else {
                self.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints)
            };
        let max_inline = min_inline.max(specified_max_inline);
        let min_block = if style.min_height().is_auto() {
            CssPixels::default()
        } else {
            self.calculate_inner_block_size(node, available_space, style.min_height(), constraints)
        };
        let specified_max_block =
            if self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
                input_block_size
            } else {
                self.calculate_inner_block_size(node, available_space, style.max_height(), constraints)
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
                    min_block.max(self.content_block_size_from_aspect_ratio(node, max_inline)),
                );
            }
            if input_inline_size > max_inline && input_block_size > max_block && !max_inline_fraction_le_max_block {
                return (
                    min_inline.max(self.content_inline_size_from_aspect_ratio(node, max_block)),
                    max_block,
                );
            }
            let min_inline_fraction_le_min_block = (min_inline.raw_value() as i64)
                * (input_block_size.raw_value() as i64)
                <= (min_block.raw_value() as i64) * (input_inline_size.raw_value() as i64);
            if input_inline_size < min_inline && input_block_size < min_block && min_inline_fraction_le_min_block {
                return (
                    max_inline.min(self.content_inline_size_from_aspect_ratio(node, min_block)),
                    min_block,
                );
            }
            if input_inline_size < min_inline && input_block_size < min_block && !min_inline_fraction_le_min_block {
                return (
                    min_inline,
                    max_block.min(self.content_block_size_from_aspect_ratio(node, min_inline)),
                );
            }
        }
        if input_inline_size > max_inline {
            return (
                max_inline,
                self.content_block_size_from_aspect_ratio(node, max_inline)
                    .max(min_block),
            );
        }
        if input_inline_size < min_inline {
            return (
                min_inline,
                self.content_block_size_from_aspect_ratio(node, min_inline)
                    .min(max_block),
            );
        }
        if input_block_size > max_block {
            return (
                self.content_inline_size_from_aspect_ratio(node, max_block)
                    .max(min_inline),
                max_block,
            );
        }
        if input_block_size < min_block {
            return (
                self.content_inline_size_from_aspect_ratio(node, min_block)
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
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> (&'static ComputedSize, &'static ComputedSize) {
        let style = self.style(node);
        let inline = if self.should_treat_inline_size_as_auto(node, available_space) {
            auto_computed_size()
        } else {
            style.width()
        };
        let block = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            auto_computed_size()
        } else {
            style.height()
        };
        (inline, block)
    }

    pub(crate) fn compute_inline_size_for_replaced_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.3.4 Block-level, replaced elements in normal flow...
        // 10.3.2 Inline, replaced elements
        let style = self.style(node);
        let (computed_inline, computed_block) =
            self.computed_sizes_for_replaced_element(node, available_space, constraints);
        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut used =
            self.tentative_inline_size_for_replaced_element(node, computed_inline, available_space, constraints);
        if computed_inline.is_auto() && computed_block.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            let block =
                self.tentative_block_size_for_replaced_element(node, computed_block, available_space, constraints);
            used = self
                .solve_replaced_size_constraint(node, used, block, available_space, constraints)
                .0;
        }
        // 2. If the tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let max =
                self.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints);
            if used > max {
                used = self.tentative_inline_size_for_replaced_element(
                    node,
                    style.max_width(),
                    available_space,
                    constraints,
                );
            }
        }
        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        if !style.min_width().is_auto() {
            let min =
                self.calculate_inner_inline_size(node, available_space.inline_size, style.min_width(), constraints);
            if used < min {
                used = self.tentative_inline_size_for_replaced_element(
                    node,
                    style.min_width(),
                    available_space,
                    constraints,
                );
            }
        }
        used
    }

    pub(crate) fn compute_block_size_for_replaced_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.6.2 Inline replaced elements
        // 10.6.4 Block-level replaced elements in normal flow
        // 10.6.6 Floating replaced elements
        // 10.6.10 'inline-block' replaced elements in normal flow
        let style = self.style(node);
        let (computed_inline, computed_block) =
            self.computed_sizes_for_replaced_element(node, available_space, constraints);
        // 1. The tentative used height is calculated (without 'min-height' and 'max-height')
        let mut used =
            self.tentative_block_size_for_replaced_element(node, computed_block, available_space, constraints);
        if computed_inline.is_auto() && computed_block.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            // However, for replaced elements with both 'width' and 'height' computed as 'auto',
            // use the algorithm under 'Minimum and maximum widths'
            // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
            // to find the used width and height.
            let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
            if intrinsic.width.is_some() || intrinsic.height.is_none() {
                // NOTE: This is a special case where calling tentative_inline_size_for_replaced_element() would call us right back,
                //       and we'd end up in an infinite loop. So we need to handle this case separately.
                let inline = self.tentative_inline_size_for_replaced_element(
                    node,
                    computed_inline,
                    available_space,
                    constraints,
                );
                used = self
                    .solve_replaced_size_constraint(node, inline, used, available_space, constraints)
                    .1;
            }
        }
        // 2. If this tentative height is greater than 'max-height', the rules above are applied again,
        //    but this time using the value of 'max-height' as the computed value for 'height'.
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
            let max = self.calculate_inner_block_size(node, available_space, style.max_height(), constraints);
            if used > max {
                used = self.tentative_block_size_for_replaced_element(
                    node,
                    style.max_height(),
                    available_space,
                    constraints,
                );
            }
        }
        // 3. If the resulting height is smaller than 'min-height', the rules above are applied again,
        //    but this time using the value of 'min-height' as the computed value for 'height'.
        if !style.min_height().is_auto() {
            let min = self.calculate_inner_block_size(node, available_space, style.min_height(), constraints);
            if used < min {
                used = self.tentative_block_size_for_replaced_element(
                    node,
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
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
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
            if self.should_treat_inline_size_as_auto(node, available_space)
                && self.should_treat_block_size_as_auto(node, available_space, constraints)
                && !facts.has_auto_content_width()
                && !facts.has_auto_content_height()
            {
                return false;
            }
            return true;
        }
        false
    }

    pub(crate) fn constraints_for_child_context(
        &self,
        containing_block: Node,
        constraints: ContainingBlockConstraints,
    ) -> ContainingBlockConstraints {
        let facts = self.facts(containing_block);
        let style = self.style(containing_block);
        let used = self.used(containing_block);
        // Anonymous boxes are invisible to percentage resolution: their children resolve percentages
        // against the closest non-anonymous ancestor, so an anonymous containing block without a
        // definite size of its own passes the constraints it was given through. Anonymous table
        // cells are the exception: they are proper containing blocks with their own size semantics.
        let should_forward_indefinite_basis = facts.is_box()
            && facts.is_anonymous()
            && !facts.is_table_cell()
            && !facts.has_auto_content_box_size()
            && used.inline_size_constraint.get() == SizeConstraint::None
            && used.block_size_constraint.get() == SizeConstraint::None;

        let inline = if used.has_definite_inline_size() {
            Some(used.content_inline_size.get())
        } else if should_forward_indefinite_basis {
            constraints.percentage_basis_inline_size
        } else {
            None
        };
        let block = if used.has_definite_block_size() {
            Some(used.content_block_size.get())
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
            Some(used.content_block_size.get())
        } else if facts.is_table_cell() {
            Some(CssPixels::default())
        } else if !style.height().is_auto()
            || facts.is_absolutely_positioned()
            || !facts.is_block_container()
            || facts.is_table_wrapper()
        {
            Some(used.content_block_size.get())
        } else {
            constraints.quirks_mode_percentage_basis_block_size
        };

        let forwarded_a_block_basis = (!used.has_definite_block_size()
            && should_forward_indefinite_basis
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
        let forwards_block_basis_as_anonymous_box = facts.is_box()
            && facts.is_anonymous()
            && !facts.is_table_cell()
            && !facts.has_auto_content_box_size()
            && self.records.used_values_if_owned(node).is_some_and(|used| {
                used.inline_size_constraint.get() == SizeConstraint::None
                    && used.block_size_constraint.get() == SizeConstraint::None
            });
        let forwards_quirks_basis_as_auto_height_block_container = in_quirks_mode
            && !facts.is_viewport()
            && !facts.is_table_cell()
            && style.height().is_auto()
            && !facts.is_absolutely_positioned()
            && facts.is_block_container()
            && !facts.is_table_wrapper();
        svg_root_forwards_quirks_basis
            || forwards_block_basis_as_anonymous_box
            || forwards_quirks_basis_as_auto_height_block_container
    }

    pub(crate) fn resolve_percentage_block_size_dependency(&self, node: Node) -> bool {
        let used = self.used(node);
        let depends_on_percentage_block_size = used.depends_on_percentage_block_size.get()
            || self.own_style_depends_on_percentage_block_size(node)
            || (used.has_descendant_that_depends_on_percentage_block_size.get()
                && self.passes_percentage_block_size_through_to_children(node));
        used.depends_on_percentage_block_size
            .set(depends_on_percentage_block_size);
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
    fn measurement_root_observes_percentage_inline_basis(&self, node: Node) -> bool {
        let facts = self.facts(node);
        if facts.is_anonymous() || facts.is_replaced_box() || facts.has_auto_content_box_size() {
            return true;
        }
        let style = self.style(node);
        style.width().contains_percentage()
            || style.min_width().contains_percentage()
            || style.max_width().contains_percentage()
            || style.padding_left().contains_percentage()
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
        if let Some(record) = self.records.used_values_if_owned(node) {
            record.depends_on_percentage_block_size.set(true);
        }
        formatting_context::propagate_percentage_block_size_dependency_to_containing_block(
            &self.records,
            &self.callbacks,
            node,
            true,
        );
    }

    pub(crate) fn should_treat_inline_size_as_auto(&self, node: Node, available_space: AvailableSpace) -> bool {
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
            if !facts.has_auto_content_height() {
                return true;
            }
            // If the box has definite height, we can resolve the width through the aspect ratio.
            if self.used(node).has_definite_block_size() {
                return true;
            }
        }
        false
    }

    pub(crate) fn should_treat_block_size_as_auto(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let style = self.style(node);
        let size = style.height();
        let facts = self.facts(node);
        if size.is_auto() {
            if self.used(node).has_definite_inline_size() && facts.has_preferred_aspect_ratio() {
                return false;
            }
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage() {
            match formatting_context::cyclic_percentage_intrinsic_contribution(
                facts.is_replaced_box(),
                true,
                available_space.block_size,
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
            // https://www.w3.org/TR/CSS22/visudet.html#the-height-property
            // If the height of the containing block is not specified explicitly (i.e., it depends on
            // content height), and this element is not absolutely positioned, the percentage value
            // is treated as 'auto'.
            // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
            // In quirks mode, percentage heights can resolve even without explicit containing block
            // height. The quirk applies to DOM elements only (not anonymous boxes), and excludes
            // table-related display types.
            if !facts.is_absolutely_positioned() {
                let parent = self.parent(node);
                let parent_is_flex_or_grid = if parent.is_invalid() {
                    false
                } else {
                    let display = self.facts(parent).display();
                    display.is_flex_inside() || display.is_grid_inside()
                };
                // Flex/grid items resolve percentage heights against their container, not via quirk.
                // The quirk should not apply inside user agent shadow trees.
                let quirk_applies = facts.document_in_quirks_mode()
                    && !facts.is_anonymous()
                    && !facts.is_table_box()
                    && !parent_is_flex_or_grid
                    && !facts.is_in_user_agent_shadow_tree();
                if !quirk_applies && constraints.percentage_basis_block_size.is_none() {
                    return true;
                }
            }
        }
        // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for height...
        if facts.has_preferred_aspect_ratio() && size.is_intrinsic_sizing_constraint() {
            // If the box has no natural width to resolve the aspect ratio, we treat the height as auto.
            if !facts.has_auto_content_width() {
                return true;
            }
            // If the box has definite width, we can resolve the height through the aspect ratio.
            if self.used(node).has_definite_inline_size() {
                return true;
            }
        }
        false
    }

    pub(crate) fn should_treat_max_inline_size_as_none(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let size = self.style(node).max_width();
        if size.is_none() || (available == AvailableSize::MaxContent && size.is_max_content()) {
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
        (size.is_fit_content() && available.is_intrinsic_sizing_constraint())
            || (size.is_max_content() && available == AvailableSize::MaxContent)
            || (size.is_min_content() && available == AvailableSize::MinContent)
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
        if size.contains_percentage() {
            if available == AvailableSize::MinContent {
                return false;
            }
            if constraints.percentage_basis_block_size.is_none() {
                return true;
            }
        }
        (size.is_fit_content() && available.is_intrinsic_sizing_constraint())
            || (size.is_max_content() && available == AvailableSize::MaxContent)
            || (size.is_min_content() && available == AvailableSize::MinContent)
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
        let mut resolution_space = available_space;
        resolution_space.block_size =
            AvailableSize::definite(constraints.quirks_mode_percentage_basis_block_size.unwrap_or_default());
        resolution_space
    }

    fn clamp_block_size_to_min_max(
        &self,
        node: Node,
        mut block_size: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let style = self.style(node);
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            let max = self.calculate_inner_block_size(node, available_space, style.max_height(), constraints);
            block_size = block_size.min(max);
        }
        if !style.min_height().is_auto() {
            let min = self.calculate_inner_block_size(node, available_space, style.min_height(), constraints);
            block_size = block_size.max(min);
        }
        block_size
    }

    pub(crate) fn resolve_used_block_size_if_not_treated_as_auto(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            return;
        }
        let style = self.style(node);
        let block_size = self.calculate_inner_block_size(node, available_space, style.height(), constraints);
        let block_size = self.clamp_block_size_to_min_max(node, block_size, available_space, constraints);
        let used = self.used(node);
        used.set_content_block_size(block_size);
        if !style.height().is_intrinsic_sizing_constraint() {
            used.has_definite_block_size.set(true);
        }
    }

    pub(crate) fn resolve_used_block_size_if_treated_as_auto(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        child_automatic_block_size: Option<CssPixels>,
        automatic_block_size_fallback: impl FnOnce() -> CssPixels,
    ) {
        if !self.should_treat_block_size_as_auto(node, available_space, constraints) {
            return;
        }
        let style = self.style(node);
        let facts = self.facts(node);
        let box_is_sized_as_replaced_element =
            self.box_is_sized_as_replaced_element(node, available_space, constraints);
        let block_size_is_definite_from_aspect_ratio = self.used(node).has_definite_inline_size()
            && facts.has_preferred_aspect_ratio()
            && box_is_sized_as_replaced_element;
        let mut block_size = if box_is_sized_as_replaced_element {
            self.compute_block_size_for_replaced_element(node, available_space, constraints)
        } else {
            child_automatic_block_size.unwrap_or_else(automatic_block_size_fallback)
        };
        block_size = self.clamp_block_size_to_min_max(node, block_size, available_space, constraints);

        if facts.document_in_quirks_mode() && facts.is_html_html_element() && style.height().is_auto() {
            // 3.6. The html element fills the viewport quirk
            // https://quirks.spec.whatwg.org/#the-html-element-fills-the-viewport-quirk
            // FIXME: Handle vertical writing mode.

            // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
            //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
            //    the margin-top and margin-bottom properties of element.
            let used = self.used(node);
            let margins = used.margin_top.get() + used.margin_bottom.get();
            // 2. Let size be the size of the initial containing block in the block flow direction minus margins.
            let size = constraints.block_basis() - margins;
            // 3. Return the bigger value of size and the normal border box size the element would have
            //    according to the CSS specification.
            block_size = block_size.max(size);
            // NOTE: The block size of the root element when affected by this quirk is considered to be definite.
            self.used(node).has_definite_block_size.set(true);
        }

        if facts.document_in_quirks_mode() && facts.is_html_body_element() && style.height().is_auto() {
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
                let used = self.used(node);
                let margins = used.margin_top.get() + used.margin_bottom.get();
                // 2. Let size be the size of element's parent element's content box in the block flow direction minus margins.
                let size = constraints.block_basis() - margins;
                // 3. Return the bigger value of size and the normal border box size the element would have
                //    according to the CSS specification.
                block_size = block_size.max(size);
            }
        }
        let used = self.used(node);
        used.set_content_block_size(block_size);
        if block_size_is_definite_from_aspect_ratio {
            used.has_definite_block_size.set(true);
        }
    }

    pub(crate) fn resolve_box_model_metrics_against_inline_basis(&self, node: Node, containing_inline_size: CssPixels) {
        let style = self.style(node);
        let used = self.used(node);
        used.margin_left.set(style.margin_left().to_px(containing_inline_size));
        used.border_left.set(style.border_left_width());
        used.padding_left
            .set(style.padding_left().to_px(containing_inline_size));
        used.margin_right
            .set(style.margin_right().to_px(containing_inline_size));
        used.border_right.set(style.border_right_width());
        used.padding_right
            .set(style.padding_right().to_px(containing_inline_size));
        used.margin_top.set(style.margin_top().to_px(containing_inline_size));
        used.border_top.set(style.border_top_width());
        used.padding_top.set(style.padding_top().to_px(containing_inline_size));
        used.padding_bottom
            .set(style.padding_bottom().to_px(containing_inline_size));
        used.border_bottom.set(style.border_bottom_width());
        used.margin_bottom
            .set(style.margin_bottom().to_px(containing_inline_size));
    }

    pub(crate) fn dimension_atomic_root(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        layout_mode: LayoutMode,
        report_for_run_cache_the_available_inline_sizes_this_sizing_repeats_for: bool,
    ) -> Option<CssPixels> {
        let containing_inline_size = available_space.inline_size.to_px_or_zero();
        let style = self.style(node);
        self.resolve_box_model_metrics_against_inline_basis(node, containing_inline_size);

        let facts = self.facts(node);
        if self.box_is_sized_as_replaced_element(node, available_space, constraints) {
            let inline_size = self.compute_inline_size_for_replaced_element(node, available_space, constraints);
            self.used(node).set_content_inline_size(inline_size);

            let block_size = self.compute_block_size_for_replaced_element(node, available_space, constraints);
            self.used(node).set_content_block_size(block_size);

            // Native form controls lay out their internal shadow contents against the concrete size produced by
            // replaced sizing. That size remains definite when a min/max constraint clamps the automatic preferred
            // height, unless the preferred height is itself an intrinsic sizing constraint.
            let native_control_block_size_constraint_is_active = facts.is_native_form_control_box()
                && facts.has_auto_content_height()
                && !style.height().is_intrinsic_sizing_constraint()
                && ((!style.min_height().is_auto() && block_size > facts.auto_content_height())
                    || (!style.max_height().is_auto()
                        && !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
                        && block_size < facts.auto_content_height()));

            let block_size_is_automatic =
                style.height().is_auto() || self.should_treat_block_size_as_auto(node, available_space, constraints);
            let block_size_is_definite_from_aspect_ratio = self.used(node).has_definite_inline_size()
                && facts.has_preferred_aspect_ratio()
                && block_size_is_automatic;

            if native_control_block_size_constraint_is_active || block_size_is_definite_from_aspect_ratio {
                self.used(node).has_definite_block_size.set(true);
            }
            return None;
        }

        let resolution = self.calculate_atomic_root_content_inline_size(node, available_space, constraints, None);
        self.used(node).set_content_inline_size(resolution.content_inline_size);
        let sizing_repeats_for_available_inline_sizes_at_or_above =
            report_for_run_cache_the_available_inline_sizes_this_sizing_repeats_for
                .then(|| {
                    self.available_inline_size_at_or_above_which_this_atomic_root_sizing_repeats(
                        node,
                        available_space,
                        &resolution,
                    )
                })
                .flatten();

        let inline_definite_space = AvailableSpace {
            inline_size: AvailableSize::definite(resolution.content_inline_size),
            block_size: AvailableSize::Indefinite,
        };
        self.resolve_used_block_size_if_not_treated_as_auto(node, inline_definite_space, constraints);
        self.make_button_content_box_definite(node, layout_mode, available_space, constraints, None);
        sizing_repeats_for_available_inline_sizes_at_or_above
    }

    fn available_inline_size_at_or_above_which_this_atomic_root_sizing_repeats(
        &self,
        node: Node,
        available_space: AvailableSpace,
        resolution: &AtomicRootInlineSizeResolution,
    ) -> Option<CssPixels> {
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
        Some(max_content_size + self.used(node).horizontal_margin_border_padding())
    }

    fn calculate_atomic_root_content_inline_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        intrinsic_content_inline_size: Option<CssPixels>,
    ) -> AtomicRootInlineSizeResolution {
        let style = self.style(node);
        let mut max_content_size_that_fit_the_definite_available_inner_space = None;
        let width_was_treated_as_auto = self.should_treat_inline_size_as_auto(node, available_space);
        let unconstrained_inline_size = if width_was_treated_as_auto {
            if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                let available =
                    available_space.inline_size.to_px_or_zero() - self.used(node).horizontal_margin_border_padding();
                let preferred = self.calculate_max_content_inline_size(node, constraints);
                if preferred <= available {
                    max_content_size_that_fit_the_definite_available_inner_space = Some(preferred);
                    preferred
                } else {
                    self.calculate_min_content_inline_size(node, constraints)
                        .max(available)
                        .min(preferred)
                }
            } else if available_space.inline_size == AvailableSize::MinContent {
                intrinsic_content_inline_size
                    .unwrap_or_else(|| self.calculate_min_content_inline_size(node, constraints))
            } else {
                intrinsic_content_inline_size
                    .unwrap_or_else(|| self.calculate_max_content_inline_size(node, constraints))
            }
        } else if style.width().contains_percentage()
            && !matches!(available_space.inline_size, AvailableSize::Definite(_))
        {
            CssPixels::default()
        } else {
            self.calculate_inner_inline_size(node, available_space.inline_size, style.width(), constraints)
        };

        let mut inline_size = unconstrained_inline_size;
        if !self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            inline_size = inline_size.min(self.calculate_inner_inline_size(
                node,
                available_space.inline_size,
                style.max_width(),
                constraints,
            ));
        }
        if !style.min_width().is_auto() {
            inline_size = inline_size.max(self.calculate_inner_inline_size(
                node,
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

    pub(crate) fn paired_min_content_inline_size_for_atomic_root(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        if available_space.inline_size != AvailableSize::MaxContent
            || self.box_is_sized_as_replaced_element(node, available_space, constraints)
        {
            return None;
        }
        let min_content_inline_size = if self.has_children(node) {
            self.intrinsic_inline_measurement_cache_get(
                node,
                IntrinsicSizeCacheKind::MaxContentInline,
                formatting_context::cache_key(None, None, constraints),
            )?
            .min_content_inline_size_from_max_content_layout?
        } else {
            CssPixels::default()
        };
        Some(
            self.calculate_atomic_root_content_inline_size(
                node,
                AvailableSpace {
                    inline_size: AvailableSize::MinContent,
                    ..available_space
                },
                constraints,
                Some(min_content_inline_size),
            )
            .content_inline_size,
        )
    }

    fn calculate_stretch_fit_inline_size(&self, node: Node, available: AvailableSize) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
        // The size a box would take if its outer size filled the available space in the given axis;
        // in other words, the stretch fit into the available space, if that is definite.
        //
        // Undefined if the available space is indefinite.
        if !matches!(available, AvailableSize::Definite(_)) {
            return CssPixels::default();
        }
        available.to_px_or_zero() - self.used(node).horizontal_margin_border_padding()
    }

    fn calculate_stretch_fit_block_size(&self, node: Node, available: AvailableSize) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
        // The size a box would take if its outer size filled the available space in the given axis;
        // in other words, the stretch fit into the available space, if that is definite.
        // Undefined if the available space is indefinite.
        let used = self.used(node);
        available.to_px_or_zero()
            - used.margin_top.get()
            - used.margin_bottom.get()
            - used.padding_top.get()
            - used.padding_bottom.get()
            - used.border_top.get()
            - used.border_bottom.get()
    }

    fn intrinsic_block_cache_get(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicBlockSizeMeasurement> {
        let measurement =
            self.callbacks
                .arena()
                .intrinsic_block_size_cache_get(self.callbacks.node_data(node), kind, key)?;
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
        self.callbacks
            .arena()
            .intrinsic_block_size_cache_put(self.callbacks.node_data(node), kind, key, value);
    }

    fn intrinsic_inline_measurement_cache_get(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicInlineSizeMeasurement> {
        let measurement = self.callbacks.arena().intrinsic_inline_size_measurement_cache_get(
            self.callbacks.node_data(node),
            kind,
            key,
        )?;
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            measurement.depends_on_percentage_block_size,
        );
        Some(measurement)
    }

    fn intrinsic_inline_measurement_cache_put(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicInlineSizeMeasurement,
    ) {
        self.callbacks.arena().intrinsic_inline_size_measurement_cache_put(
            self.callbacks.node_data(node),
            kind,
            key,
            value,
        );
    }

    #[allow(clippy::too_many_arguments)]
    fn cache_intrinsic_inline_measurement(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        measurement_root_used: &UsedValues,
        result: formatting_context::ChildLayoutResult,
        available_block_size: AvailableSize,
    ) {
        let used = measurement_root_used;
        self.intrinsic_inline_measurement_cache_put(
            node,
            kind,
            key,
            IntrinsicInlineSizeMeasurement {
                automatic_content_inline_size: result.automatic_content_inline_size,
                min_content_inline_size_from_max_content_layout: result.min_content_inline_size_from_max_content_layout,
                available_block_size,
                content_inline_size: used.content_inline_size.get(),
                content_block_size: used.content_block_size.get(),
                automatic_content_block_size: result.automatic_content_block_size,
                uses_collapsing_borders_model: used.uses_collapsing_borders_model.get(),
                has_first_baseline: used.has_first_baseline.get(),
                first_baseline: used.first_baseline.get(),
                has_last_baseline: used.has_last_baseline.get(),
                last_baseline: used.last_baseline.get(),
                depends_on_percentage_block_size: result.depends_on_percentage_block_size,
                depends_on_percentage_inline_basis: self.measurement_root_observes_percentage_inline_basis(node),
            },
        );
    }

    pub(crate) fn apply_cached_intrinsic_inline_measurement(
        &self,
        node: Node,
        available_inline_size: AvailableSize,
        available_block_size: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> Option<(CssPixels, DerivedBaselines)> {
        // OPTIMIZATION: Calculating an intrinsic inline size already performs a complete measurement layout.
        // A later equivalent intrinsic line build only consumes the atomic box's measured dimensions and
        // baselines, so retain that summary instead of formatting the same descendants again. Commit layout
        // must still create all descendant geometry.
        if !self.purpose.is_measurement() {
            return None;
        }
        let kind = match available_inline_size {
            AvailableSize::MinContent => IntrinsicSizeCacheKind::MinContentInline,
            AvailableSize::MaxContent => IntrinsicSizeCacheKind::MaxContentInline,
            AvailableSize::Definite(_) | AvailableSize::Indefinite => return None,
        };
        let measurement = self.intrinsic_inline_measurement_cache_get(
            node,
            kind,
            formatting_context::cache_key(None, None, constraints),
        )?;
        if measurement.available_block_size != available_block_size {
            return None;
        }
        let used = self.used(node);
        if used.content_inline_size.get() != measurement.content_inline_size {
            return None;
        }

        used.set_content_block_size(measurement.content_block_size);
        used.uses_collapsing_borders_model
            .set(measurement.uses_collapsing_borders_model);
        let baselines = DerivedBaselines {
            first: measurement.has_first_baseline.then_some(measurement.first_baseline),
            last: measurement.has_last_baseline.then_some(measurement.last_baseline),
        };
        formatting_context::store_derived_baselines(&used, baselines);
        Some((measurement.automatic_content_block_size, baselines))
    }

    fn calculate_transferred_inline_size_for_replaced_element(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        // "size constraints in the opposite dimension will transfer through and can affect the auto size in the considered one"
        let facts = self.facts(node);
        let style = self.style(node);
        // https://drafts.csswg.org/css2/#inline-replaced-width
        // "'width' has a computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic ratio"
        if !facts.is_replaced_box()
            || !facts.has_preferred_aspect_ratio()
            || !style.width().is_auto()
            || style.height().is_auto()
            || style.height().is_intrinsic_sizing_constraint()
        {
            return None;
        }
        let available_space = self
            .used(node)
            .available_inner_space_or_constraints_from(AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: AvailableSize::Indefinite,
            });
        if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            return None;
        }
        // https://drafts.csswg.org/css2/#inline-replaced-width
        // "(used height) * (intrinsic ratio)"
        Some(self.compute_inline_size_for_replaced_element(
            node,
            available_space,
            ContainingBlockConstraints::default(),
        ))
    }

    pub(crate) fn calculate_min_content_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.calculate_min_content_inline_size_with_block_size(node, constraints, None)
    }

    pub(crate) fn calculate_min_content_inline_size_at_definite_block_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: CssPixels,
    ) -> CssPixels {
        self.calculate_min_content_inline_size_with_block_size(node, constraints, Some(block_size))
    }

    fn calculate_min_content_inline_size_with_block_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> CssPixels {
        let facts = self.facts(node);
        let style = self.style(node);
        if facts.is_replaced_box() && (style.width().contains_percentage() || style.max_width().contains_percentage()) {
            // https://www.w3.org/TR/css-sizing-3/#replaced-percentage-min-contribution
            // NOTE: If the box is replaced, a cyclic percentage in the value of any max size property or
            //       preferred size property (width/max-width/height/max-height), is resolved against zero
            //       when calculating the min-content contribution in the corresponding axis.
            // FIXME: If the box also has a preferred aspect ratio, then this min-content contribution is
            //        floored by any <length-percentage> minimum size from the opposite axis—resolving any
            //        such percentage against zero—transferred through the preferred aspect ratio.
            // Note: The min-content contribution is, as always, also floored by the minimum size in its own axis.
            if !style.min_width().is_length_percentage() {
                return CssPixels::default();
            }
            let mut zero_constraints = constraints;
            zero_constraints.percentage_basis_inline_size = Some(CssPixels::default());
            return self.calculate_inner_inline_size(
                node,
                AvailableSize::MinContent,
                style.min_width(),
                zero_constraints,
            );
        }
        if let Some(transferred) = self.calculate_transferred_inline_size_for_replaced_element(node, constraints) {
            return transferred;
        }
        let auto_size = self.auto_content_size(node);
        if let Some(width) = auto_size.width {
            return width;
        }
        if facts.is_replaced_box()
            && !facts.has_preferred_aspect_ratio()
            && let Some(fallback) = self.max_content_size_for_replaced_element_without_natural_size(
                node,
                auto_size,
                formatting_context::SizeDimension::Inline,
                formatting_context::ReplacedMaxContentSizeConstraints::default(),
            )
        {
            return fallback;
        }
        // Boxes with no children have zero intrinsic inline size.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        if let Some(cached) = self.intrinsic_inline_measurement_cache_get(
            node,
            IntrinsicSizeCacheKind::MinContentInline,
            formatting_context::cache_key(
                None,
                self.block_size_for_intrinsic_inline_measurement_cache_key(node, block_size),
                constraints,
            ),
        ) {
            return cached.automatic_content_inline_size;
        }
        if let Some(min_content_inline_size) = self.paired_min_content_inline_size(node, constraints, block_size) {
            return min_content_inline_size;
        }
        self.measure_intrinsic_inline_size(node, constraints, block_size, IntrinsicSizeCacheKind::MinContentInline)
    }

    fn paired_min_content_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> Option<CssPixels> {
        self.intrinsic_inline_measurement_cache_get(
            node,
            IntrinsicSizeCacheKind::MaxContentInline,
            formatting_context::cache_key(
                None,
                self.block_size_for_intrinsic_inline_measurement_cache_key(node, block_size),
                constraints,
            ),
        )?
        .min_content_inline_size_from_max_content_layout
    }

    pub(crate) fn calculate_max_content_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.calculate_max_content_inline_size_with_block_size(node, constraints, None)
    }

    pub(crate) fn calculate_max_content_inline_size_at_definite_block_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: CssPixels,
    ) -> CssPixels {
        self.calculate_max_content_inline_size_with_block_size(node, constraints, Some(block_size))
    }

    fn calculate_max_content_inline_size_with_block_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
    ) -> CssPixels {
        let facts = self.facts(node);
        let style = self.style(node);
        let mut auto_size = self.auto_content_size(node);
        if let Some(transferred) = self.calculate_transferred_inline_size_for_replaced_element(node, constraints) {
            return transferred;
        }
        if auto_size.width.is_none() && (facts.has_default_preferred_width() || facts.has_default_preferred_height()) {
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
                width: facts
                    .has_default_preferred_width()
                    .then_some(facts.default_preferred_width()),
                height: facts
                    .has_default_preferred_height()
                    .then_some(facts.default_preferred_height()),
                aspect_ratio: None,
            };
        }
        if let Some(width) = auto_size.width {
            return width;
        }
        let definite_block_size =
            if facts.is_replaced_box() && auto_size.height.is_none() && self.used(node).has_definite_block_size() {
                Some(self.used(node).content_block_size.get())
            } else {
                None
            };
        let max_content_available = AvailableSize::MaxContent;
        let intrinsic_available_space = AvailableSpace {
            inline_size: max_content_available,
            block_size: AvailableSize::Indefinite,
        };
        let resolve_destination_inline_size =
            |size: &ComputedSize, property: formatting_context::CyclicPercentageSizeProperty| -> Option<CssPixels> {
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
                        Some(self.calculate_inner_inline_size(node, max_content_available, size, zero_constraints))
                    }
                    formatting_context::CyclicPercentageIntrinsicContribution::NotCyclic => {
                        if size.contains_percentage() && constraints.percentage_basis_inline_size.is_none() {
                            None
                        } else {
                            Some(self.calculate_inner_inline_size(node, max_content_available, size, constraints))
                        }
                    }
                }
            };
        let resolve_block_size =
            |size: &ComputedSize, property: formatting_context::CyclicPercentageSizeProperty| -> Option<CssPixels> {
                if !size.is_length_percentage() {
                    return None;
                }
                if !size.contains_percentage() || constraints.percentage_basis_block_size.is_some() {
                    return Some(self.calculate_inner_block_size(node, intrinsic_available_space, size, constraints));
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
                        Some(self.calculate_inner_block_size(node, intrinsic_available_space, size, zero_constraints))
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
        let replaced_constraints = formatting_context::ReplacedMaxContentSizeConstraints {
            definite_size_in_ratio_determining_axis: definite_block_size,
            minimum_inline_size: definite_minimum_inline_size,
            minimum_block_size: definite_minimum_block_size,
        };
        if let Some(max_content_inline_size) = self.max_content_size_for_replaced_element_without_natural_size(
            node,
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
                let mut transferred_minimum =
                    definite_minimum_block_size.map(|value| self.content_inline_size_from_aspect_ratio(node, value));
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
                    self.content_inline_size_from_aspect_ratio(node, definite_maximum_block_size);
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
                return max_content_inline_size.min(transferred_maximum);
            }
            return max_content_inline_size;
        }
        // Boxes with no children have zero intrinsic inline size.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        self.measure_intrinsic_inline_size(node, constraints, block_size, IntrinsicSizeCacheKind::MaxContentInline)
    }

    // Lays `node` out under an intrinsic inline-size constraint, reusing and populating the measurement cache.
    fn measure_intrinsic_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
        block_size: Option<CssPixels>,
        kind: IntrinsicSizeCacheKind,
    ) -> CssPixels {
        let (size_constraint, available_inline_size) = match kind {
            IntrinsicSizeCacheKind::MinContentInline => (SizeConstraint::MinContent, AvailableSize::MinContent),
            IntrinsicSizeCacheKind::MaxContentInline => (SizeConstraint::MaxContent, AvailableSize::MaxContent),
            _ => unreachable!("inline measurement cache kind must use the inline axis"),
        };
        let key = formatting_context::cache_key(
            None,
            self.block_size_for_intrinsic_inline_measurement_cache_key(node, block_size),
            constraints,
        );
        if let Some(cached) = self.intrinsic_inline_measurement_cache_get(node, kind, key) {
            return cached.automatic_content_inline_size;
        }

        self.callbacks.arena().note_intrinsic_measurement();
        let measurement = formatting_context::MeasurementState::create(self.callbacks);
        let root = measurement.create_used_values(node, constraints);
        // NB: A parent layout can assign a definite block size that is not present in computed style,
        //     such as the stretched cross size of a flex item. Preserve it so descendant percentages
        //     resolve against the same size during intrinsic measurement.
        if let Some(block_size) = block_size {
            root.set_content_block_size(block_size);
            root.has_definite_block_size.set(true);
        }
        root.inline_size_constraint.set(size_constraint);
        root.has_definite_inline_size.set(false);
        let block_size = if root.has_definite_block_size() {
            AvailableSize::definite(root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        };
        let mut result = measurement.run(
            node,
            &root,
            LayoutInput::new(
                AvailableSpace {
                    inline_size: available_inline_size,
                    block_size,
                },
                constraints,
                ParticipationInParentFormattingContext::Root,
            ),
        );
        result.automatic_content_inline_size = clamp_to_max_dimension_value(result.automatic_content_inline_size);
        result.min_content_inline_size_from_max_content_layout = result
            .min_content_inline_size_from_max_content_layout
            .map(clamp_to_max_dimension_value);
        let value = result.automatic_content_inline_size;
        self.charge_measurement_dependency_to_measured_box_and_containing_block(
            node,
            result.depends_on_percentage_block_size,
        );
        self.cache_intrinsic_inline_measurement(node, kind, key, &root, result, block_size);
        value
    }

    // Lays `node` out at `inline_size` under an intrinsic block-size constraint, reusing and populating the cache.
    fn measure_intrinsic_block_size(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
        kind: IntrinsicSizeCacheKind,
    ) -> CssPixels {
        let (size_constraint, available_block_size) = match kind {
            IntrinsicSizeCacheKind::MinContentBlock => (SizeConstraint::MinContent, AvailableSize::MinContent),
            IntrinsicSizeCacheKind::MaxContentBlock => (SizeConstraint::MaxContent, AvailableSize::MaxContent),
            _ => unreachable!("block size cache kind must use the block axis"),
        };
        let key = formatting_context::cache_key(Some(inline_size), None, constraints);
        if let Some(cached) = self.intrinsic_block_cache_get(node, kind, key) {
            return cached.size;
        }

        self.callbacks.arena().note_intrinsic_measurement();
        let measurement = formatting_context::MeasurementState::create(self.callbacks);
        let root = measurement.create_used_values(node, constraints);
        root.block_size_constraint.set(size_constraint);
        root.has_definite_block_size.set(false);
        root.set_content_inline_size(inline_size);
        let result = measurement.run(
            node,
            &root,
            LayoutInput::new(
                AvailableSpace {
                    inline_size: AvailableSize::definite(inline_size),
                    block_size: available_block_size,
                },
                constraints,
                ParticipationInParentFormattingContext::Root,
            ),
        );
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

    pub(crate) fn calculate_min_content_block_size(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // https://www.w3.org/TR/css-sizing-3/#min-content-block-size
        let facts = self.facts(node);
        // For block containers, tables, and inline boxes, this is equivalent to the max-content block size.
        if facts.is_block_container() || facts.is_table_box() {
            return self.calculate_max_content_block_size(node, inline_size, constraints);
        }
        let auto_size = self.auto_content_size(node);
        if let Some(height) = auto_size.height {
            return auto_size.aspect_ratio.map_or(height, |ratio| ratio.divide(inline_size));
        }
        // Boxes with no children have zero intrinsic height.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        self.measure_intrinsic_block_size(node, inline_size, constraints, IntrinsicSizeCacheKind::MinContentBlock)
    }

    pub(crate) fn calculate_max_content_block_size(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        if let Some(ratio) = self.facts(node).preferred_aspect_ratio() {
            return ratio.divide(inline_size);
        }
        let auto_size = self.auto_content_size(node);
        if let Some(height) = auto_size.height {
            return height;
        }
        if let Some(fallback) = self.max_content_size_for_replaced_element_without_natural_size(
            node,
            auto_size,
            formatting_context::SizeDimension::Block,
            formatting_context::ReplacedMaxContentSizeConstraints::default(),
        ) {
            return fallback;
        }
        // Boxes with no children have zero intrinsic height.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        self.measure_intrinsic_block_size(node, inline_size, constraints, IntrinsicSizeCacheKind::MaxContentBlock)
    }

    pub(crate) fn measure_automatic_content_block_size(
        &self,
        node: Node,
        layout_mode: LayoutMode,
        inner_available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let measurement = formatting_context::MeasurementState::create(self.callbacks);
        let node_used = measurement.create_used_values(node, constraints);
        measurement
            .run_with_layout_mode(
                node,
                &node_used,
                layout_mode,
                LayoutInput::new(
                    inner_available_space,
                    constraints,
                    ParticipationInParentFormattingContext::Root,
                ),
            )
            .automatic_content_block_size
    }

    pub(crate) fn make_button_content_box_definite(
        &self,
        node: Node,
        layout_mode: LayoutMode,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        measured_content_block_size: Option<CssPixels>,
    ) {
        let facts = self.facts(node);
        if !facts.uses_button_layout() {
            return;
        }
        // Flex/grid-inside buttons are their own flex/grid container and get no anonymous content wrapper,
        // so there is nothing to make definite for centering.
        let style = self.style(node);
        if style.display().is_flex_inside() || style.display().is_grid_inside() {
            return;
        }
        // With auto height and no min-height the content box already exactly wraps the content, so there is
        // no extra space to center within and no need to force a definite content box.
        if style.height().is_auto() && style.min_height().is_auto() {
            return;
        }
        if self.used(node).has_definite_block_size() {
            return;
        }
        let natural = measured_content_block_size.unwrap_or_else(|| {
            self.measure_automatic_content_block_size(
                node,
                layout_mode,
                self.used(node)
                    .available_inner_space_or_constraints_from(available_space),
                constraints,
            )
        });
        let mut used_block_size = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            natural
        } else {
            self.calculate_inner_block_size(node, available_space, style.height(), constraints)
        };
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            used_block_size = used_block_size.min(self.calculate_inner_block_size(
                node,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            used_block_size = used_block_size.max(self.calculate_inner_block_size(
                node,
                available_space,
                style.min_height(),
                constraints,
            ));
        }
        // Only force a definite content box when the button's used block size exceeds its content block size, so a larger
        // preferred or minimum size has room to center within. A content-sized box stays indefinite, so an intrinsic
        // keyword does not resolve percentage-sized descendants.
        if used_block_size <= natural {
            return;
        }
        let used = self.used(node);
        used.set_content_block_size(used_block_size);
        used.has_definite_block_size.set(true);
    }

    pub(crate) fn table_box_inside_wrapper(&self, wrapper: Node) -> Node {
        fn find(context: &SizingContext, parent: Node) -> Option<Node> {
            let mut child = context.first_child(parent);
            while !child.is_invalid() {
                let facts = context.facts(child);
                if facts.is_box() && facts.display().is_table_inside() {
                    return Some(child);
                }
                if let Some(table) = find(context, child) {
                    return Some(table);
                }
                child = context.next_sibling(child);
            }
            None
        }

        find(self, wrapper).expect("table wrapper must contain a table box")
    }

    // 17.5.2 Table width algorithms: the 'table-layout' property
    // https://www.w3.org/TR/CSS22/tables.html#width-layout
    pub(crate) fn compute_table_box_inline_size_inside_wrapper(
        &self,
        wrapper: Node,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
        table_wrapper_containing_block_inline_size: Option<CssPixels>,
        table_wrapper_inline_size_mode: formatting_context::TableWrapperInlineSizeMode,
    ) -> CssPixels {
        // CSS 2 says the table wrapper inline size is the border-edge inline size of the table grid box inside it.

        let style = self.style(wrapper);
        let containing_block_inline_size =
            table_wrapper_containing_block_inline_size.unwrap_or_else(|| available_space.inline_size.to_px_or_zero());

        // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
        let margin_left = style.margin_left().to_px(containing_block_inline_size);
        let margin_right = style.margin_right().to_px(containing_block_inline_size);

        // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
        let available_inline_size = containing_block_inline_size - margin_left - margin_right;
        let table_box = self.table_box_inside_wrapper(wrapper);

        let measurement = formatting_context::MeasurementState::create(self.callbacks);

        // The table wrapper is invisible to percentage resolution, so the table box gets the
        // wrapper's constraints unchanged. Callers measuring a table wrapper for grid alignment
        // pass the grid-area inline size as the wrapper's percentage basis.
        let table_constraints = table_wrapper_constraints;
        let table_used = measurement.create_used_values(table_box, table_constraints);
        let table_style = self.style(table_box);
        table_used.border_left.set(table_style.border_left_width());
        table_used.border_right.set(table_style.border_right_width());
        table_used
            .padding_left
            .set(table_style.padding_left().to_px(containing_block_inline_size));
        table_used
            .padding_right
            .set(table_style.padding_right().to_px(containing_block_inline_size));

        let table_run = FormattingContextRun {
            purpose: formatting_context::LayoutPurpose::Measurement,
            records: std::rc::Rc::new(RunRecords::new(
                measurement.callbacks().arena,
                table_box,
                table_used.clone(),
            )),
            box_: table_box,
            layout_mode: LayoutMode::IntrinsicSizing,
            callbacks: *measurement.callbacks(),
            should_collect_devtools_layout_data: false,
            treat_block_axis_percentage_insets_as_auto_beyond_root: false,
            fragments: None,
            previous_line_data: None,
        };
        let mut table = table_formatting_context::TableFormattingContext::new(&table_run);
        let table_available = table_used.available_inner_space_or_constraints_from(available_space);
        table.run_until_inline_size_calculation(
            LayoutInput::new(
                table_available,
                table_constraints,
                ParticipationInParentFormattingContext::Root,
            ),
            true,
        );

        let table_used_inline_size = table_used.border_box_inline_size(table_used.uses_collapsing_borders_model.get());
        self.records
            .store_table_inline_layout(wrapper, table.take_inline_layout());
        if table_wrapper_inline_size_mode
            == formatting_context::TableWrapperInlineSizeMode::UseTableUsedInlineSizeIfNotAuto
            && !table_style.width().is_auto()
        {
            return table_used_inline_size;
        }
        if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
            table_used_inline_size.min(available_inline_size)
        } else {
            table_used_inline_size
        }
    }

    // 17.5.3 Table height algorithms
    // https://www.w3.org/TR/CSS22/tables.html#height-layout
    pub(crate) fn compute_table_box_block_size_inside_wrapper(
        &self,
        wrapper: Node,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // The table wrapper block size should equal the block size of the table box it contains.

        let style = self.style(wrapper);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let containing_block_block_size = available_space.block_size.to_px_or_zero();

        // If 'margin-top', or 'margin-bottom' are computed as 'auto', their used value is '0'.
        let margin_top = style.margin_top().to_px(containing_block_inline_size);
        let margin_bottom = style.margin_bottom().to_px(containing_block_inline_size);

        // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
        let available_block_size = containing_block_block_size - margin_top - margin_bottom;

        let measurement = formatting_context::MeasurementState::create(self.callbacks);
        let wrapper_used = measurement.create_used_values(wrapper, table_wrapper_constraints);
        let wrapper_outputs = measurement.run_with_layout_mode(
            wrapper,
            &wrapper_used,
            LayoutMode::IntrinsicSizing,
            LayoutInput {
                available_space: self
                    .used(wrapper)
                    .available_inner_space_or_constraints_from(available_space),
                containing_block_constraints: table_wrapper_constraints,
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Root,
            },
        );

        let table_used_block_size = wrapper_outputs
            .table_box_in_wrapper_border_box_block_size
            .expect("a table wrapper's measurement run lays out the table box inside it");
        if matches!(available_space.block_size, AvailableSize::Definite(_)) {
            table_used_block_size.min(available_block_size)
        } else {
            table_used_block_size
        }
    }

    pub(crate) fn calculate_fit_content_size(
        &self,
        node: Node,
        axis: SizingAxis,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#fit-content-size
        match axis {
            SizingAxis::Inline => {
                // If the available space in a given axis is definite, equal to clamp(min-content size, stretch-fit size,
                // max-content size) (i.e. max(min-content size, min(max-content size, stretch-fit size))).
                if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                    let stretch = self.calculate_stretch_fit_inline_size(node, available_space.inline_size);
                    let max_content = self.calculate_max_content_inline_size(node, constraints);
                    if max_content <= stretch {
                        return max_content;
                    }
                    return self.calculate_min_content_inline_size(node, constraints).max(stretch);
                }
                // When sizing under a min-content constraint, equal to the min-content size.
                if available_space.inline_size == AvailableSize::MinContent {
                    return self.calculate_min_content_inline_size(node, constraints);
                }
                // Otherwise, equal to the max-content size in that axis.
                self.calculate_max_content_inline_size(node, constraints)
            }
            SizingAxis::Block => {
                let inline_size = available_space.inline_size.to_px_or_zero();
                // https://drafts.csswg.org/css-sizing-3/#fit-content-size
                // If the available space in a given axis is definite,
                // equal to clamp(min-content size, stretch-fit size, max-content size)
                // (i.e. max(min-content size, min(max-content size, stretch-fit size))).
                if matches!(available_space.block_size, AvailableSize::Definite(_)) {
                    let stretch = self.calculate_stretch_fit_block_size(node, available_space.block_size);
                    let max_content = self.calculate_max_content_block_size(node, inline_size, constraints);
                    if max_content <= stretch {
                        return max_content;
                    }
                    return self
                        .calculate_min_content_block_size(node, inline_size, constraints)
                        .max(stretch);
                }
                // When sizing under a min-content constraint, equal to the min-content size.
                if available_space.block_size == AvailableSize::MinContent {
                    return self.calculate_min_content_block_size(node, inline_size, constraints);
                }
                // Otherwise, equal to the max-content size in that axis.
                self.calculate_max_content_block_size(node, inline_size, constraints)
            }
        }
    }

    pub(crate) fn calculate_inner_inline_size(
        &self,
        node: Node,
        available: AvailableSize,
        preferred_size: &ComputedSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        assert!(!preferred_size.is_auto());
        let basis = if preferred_size.contains_percentage() {
            if let Some(basis) = constraints.percentage_basis_inline_size {
                basis
            } else {
                available.to_px_or_zero()
            }
        } else {
            available.to_px_or_zero()
        };
        if preferred_size.is_fit_content() {
            return self.calculate_fit_content_size(
                node,
                SizingAxis::Inline,
                AvailableSpace {
                    inline_size: available,
                    block_size: AvailableSize::Indefinite,
                },
                constraints,
            );
        }
        if preferred_size.is_max_content() {
            return self.calculate_max_content_inline_size(node, constraints);
        }
        if preferred_size.is_min_content() {
            return self.calculate_min_content_inline_size(node, constraints);
        }
        let value = preferred_size.to_px(basis);
        let style = self.style(node);
        if style.box_sizing() == box_sizing::BORDER_BOX {
            let used = self.used(node);
            return formatting_context::subtract_border_box_adjustment(
                value,
                style.border_left_width(),
                used.padding_left.get(),
                style.border_right_width(),
                used.padding_right.get(),
            );
        }
        value
    }

    pub(crate) fn calculate_inner_block_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        preferred_size: &ComputedSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        if preferred_size.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            return self.content_block_size_from_aspect_ratio(node, self.used(node).content_inline_size.get());
        }
        assert!(!preferred_size.is_auto());
        if preferred_size.is_fit_content() {
            return self.calculate_fit_content_size(node, SizingAxis::Block, available_space, constraints);
        }
        if preferred_size.is_max_content() {
            return self.calculate_max_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }
        if preferred_size.is_min_content() {
            return self.calculate_min_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
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
            let parent = self.parent(node);
            let parent_is_flex_or_grid = if parent.is_invalid() {
                false
            } else {
                let display = self.facts(parent).display();
                display.is_flex_inside() || display.is_grid_inside()
            };
            if facts.document_in_quirks_mode()
                && !facts.is_anonymous()
                && !parent_is_flex_or_grid
                && !facts.is_in_user_agent_shadow_tree()
            {
                basis = constraints.quirks_mode_percentage_basis_block_size.unwrap_or_default();
            } else {
                basis = constraints.block_basis();
            }
        }
        let value = preferred_size.to_px(basis);
        let style = self.style(node);
        if style.box_sizing() == box_sizing::BORDER_BOX {
            let used = self.used(node);
            return formatting_context::subtract_border_box_adjustment(
                value,
                style.border_top_width(),
                used.padding_top.get(),
                style.border_bottom_width(),
                used.padding_bottom.get(),
            );
        }
        value
    }

    pub(crate) fn calculate_inner_size_for_property(
        &self,
        node: Node,
        axis: SizingAxis,
        property: SizingProperty,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
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
                self.calculate_inner_inline_size(node, available_space.inline_size, size, constraints)
            }
            SizingAxis::Block => self.calculate_inner_block_size(node, available_space, size, constraints),
        }
    }

    pub(crate) fn should_treat_size_as_auto(
        &self,
        node: Node,
        axis: SizingAxis,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        match axis {
            SizingAxis::Inline => self.should_treat_inline_size_as_auto(node, available_space),
            SizingAxis::Block => self.should_treat_block_size_as_auto(node, available_space, constraints),
        }
    }
}
