/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub(crate) struct SizingContext<'pass> {
    state: &'pass LayoutState,
    callbacks: FfiLayoutFcCallbacks,
}

impl<'pass> SizingContext<'pass> {
    pub(crate) fn new(state: &'pass LayoutState, callbacks: FfiLayoutFcCallbacks) -> Self {
        Self { state, callbacks }
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        self.state.node_facts(&self.callbacks, node)
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        self.state.style_facts(&self.callbacks, node)
    }

    fn used(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
    }

    fn used_mut(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
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

    fn content_block_size_from_aspect_ratio(&self, node: Node, content_inline_size: CssPixels) -> CssPixels {
        let style = self.style(node);
        let used = self.used(node);
        content_block_size_from_aspect_ratio_values(
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
        content_inline_size_from_aspect_ratio_values(
            content_block_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + used.padding_left.get(),
            style.border_right_width() + used.padding_right.get(),
            style.border_top_width() + used.padding_top.get(),
            style.border_bottom_width() + used.padding_bottom.get(),
        )
    }

    fn auto_content_size(&self, node: Node) -> ReplacedIntrinsicSize {
        let facts = self.facts(node);
        ReplacedIntrinsicSize {
            width: facts.has_auto_content_width().then_some(facts.auto_content_width()),
            height: facts.has_auto_content_height().then_some(facts.auto_content_height()),
            aspect_ratio: facts.has_auto_content_aspect_ratio().then_some(PixelFraction {
                numerator: facts.auto_content_aspect_ratio_numerator(),
                denominator: facts.auto_content_aspect_ratio_denominator(),
            }),
        }
    }

    fn intrinsic_size_for_replaced_sizing(&self, node: Node) -> ReplacedIntrinsicSize {
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
        ReplacedIntrinsicSize {
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
        natural_size: ReplacedIntrinsicSize,
        dimension: SizeDimension,
        constraints: ReplacedMaxContentSizeConstraints,
    ) -> Option<CssPixels> {
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        // the intrinsic sizes of replaced elements without natural sizes are defined below:
        let facts = self.facts(node);
        let is_inline_axis = dimension == SizeDimension::Inline;
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
            match cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                style.width().contains_percentage(),
                available_space.inline_size,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    return CssPixels::default();
                }
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => {}
                CyclicPercentageIntrinsicContribution::NotCyclic => {
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

    pub(crate) fn compute_inline_size_for_replaced_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.3.4 Block-level, replaced elements in normal flow...
        // 10.3.2 Inline, replaced elements
        let style = self.style(node);
        let computed_inline = if self.should_treat_inline_size_as_auto(node, available_space) {
            auto_computed_size()
        } else {
            style.width()
        };
        let computed_block = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            auto_computed_size()
        } else {
            style.height()
        };
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
        let computed_inline = if self.should_treat_inline_size_as_auto(node, available_space) {
            auto_computed_size()
        } else {
            style.width()
        };
        let computed_block = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            auto_computed_size()
        } else {
            style.height()
        };
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
        let quirks_block = if facts.is_viewport() {
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

        ContainingBlockConstraints {
            percentage_basis_inline_size: inline,
            percentage_basis_block_size: block,
            quirks_mode_percentage_basis_block_size: quirks_block,
        }
    }

    pub(crate) fn should_treat_inline_size_as_auto(&self, node: Node, available_space: AvailableSpace) -> bool {
        let style = self.style(node);
        let size = style.width();
        if size.is_auto() {
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage() {
            match cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                true,
                available_space.inline_size,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => return false,
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => return true,
                CyclicPercentageIntrinsicContribution::NotCyclic => {}
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
            match cyclic_percentage_intrinsic_contribution(
                facts.is_replaced_box(),
                true,
                available_space.block_size,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => return false,
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => return true,
                CyclicPercentageIntrinsicContribution::NotCyclic => {}
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
            match cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                true,
                available,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => return false,
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => return true,
                CyclicPercentageIntrinsicContribution::NotCyclic => {}
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
        let mut block_size = self.calculate_inner_block_size(node, available_space, style.height(), constraints);
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            block_size = block_size.min(self.calculate_inner_block_size(
                node,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            block_size = block_size.max(self.calculate_inner_block_size(
                node,
                available_space,
                style.min_height(),
                constraints,
            ));
        }
        let used = self.used_mut(node);
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
        let mut block_size = if self.box_is_sized_as_replaced_element(node, available_space, constraints) {
            self.compute_block_size_for_replaced_element(node, available_space, constraints)
        } else {
            child_automatic_block_size.unwrap_or_else(automatic_block_size_fallback)
        };
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            block_size = block_size.min(self.calculate_inner_block_size(
                node,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            block_size = block_size.max(self.calculate_inner_block_size(
                node,
                available_space,
                style.min_height(),
                constraints,
            ));
        }

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
            self.used_mut(node).has_definite_block_size.set(true);
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
        self.used_mut(node).set_content_block_size(block_size);
    }

    pub(crate) fn resolve_box_model_metrics_against_inline_basis(&self, node: Node, containing_inline_size: CssPixels) {
        let style = self.style(node);
        let used = self.used_mut(node);
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
    ) {
        let containing_inline_size = available_space.inline_size.to_px_or_zero();
        let style = self.style(node);
        self.resolve_box_model_metrics_against_inline_basis(node, containing_inline_size);

        let facts = self.facts(node);
        if self.box_is_sized_as_replaced_element(node, available_space, constraints) {
            let inline_size = self.compute_inline_size_for_replaced_element(node, available_space, constraints);
            self.used_mut(node).set_content_inline_size(inline_size);
            let block_size = self.compute_block_size_for_replaced_element(node, available_space, constraints);
            self.used_mut(node).set_content_block_size(block_size);
            let block_size_is_automatic =
                style.height().is_auto() || self.should_treat_block_size_as_auto(node, available_space, constraints);
            if self.used(node).has_definite_inline_size() && facts.has_preferred_aspect_ratio() && block_size_is_automatic
            {
                self.used_mut(node).has_definite_block_size.set(true);
            }
            return;
        }

        let unconstrained_inline_size = if self.should_treat_inline_size_as_auto(node, available_space) {
            if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                let used = self.used(node);
                let available = available_space.inline_size.to_px_or_zero()
                    - used.margin_left.get()
                    - used.border_left.get()
                    - used.padding_left.get()
                    - used.padding_right.get()
                    - used.border_right.get()
                    - used.margin_right.get();
                let preferred = self.calculate_max_content_inline_size(node, constraints);
                if preferred <= available {
                    preferred
                } else {
                    self.calculate_min_content_inline_size(node, constraints)
                        .max(available)
                        .min(preferred)
                }
            } else if available_space.inline_size == AvailableSize::MinContent {
                self.calculate_min_content_inline_size(node, constraints)
            } else {
                self.calculate_max_content_inline_size(node, constraints)
            }
        } else if style.width().contains_percentage() && !matches!(available_space.inline_size, AvailableSize::Definite(_)) {
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
        self.used_mut(node).set_content_inline_size(inline_size);

        let inline_definite_space = AvailableSpace {
            inline_size: AvailableSize::definite(inline_size),
            block_size: AvailableSize::Indefinite,
        };
        self.resolve_used_block_size_if_not_treated_as_auto(node, inline_definite_space, constraints);
        if style.display().is_flex_inside() {
            // Flex containers with an automatic block size are treated as max-content, so resolve it early.
            self.resolve_used_block_size_if_treated_as_auto(node, inline_definite_space, constraints, None, || {
                crate::layout::independent_root_automatic_block_size(
                    self.state,
                    &self.callbacks,
                    node,
                    self.used(node)
                        .available_inner_space_or_constraints_from(inline_definite_space),
                    constraints,
                    None,
                )
            });
        }
        self.make_button_content_box_definite(node, layout_mode, available_space, constraints, None);
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
        let used = self.used(node);
        available.to_px_or_zero()
            - used.margin_left.get()
            - used.margin_right.get()
            - used.padding_left.get()
            - used.padding_right.get()
            - used.border_left.get()
            - used.border_right.get()
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
    ) -> Option<CssPixels> {
        self.callbacks
            .arena()
            .intrinsic_block_size_cache_get(self.callbacks.node_data(node), kind, key)
    }

    fn intrinsic_block_cache_put(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: CssPixels,
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
        self.callbacks.arena().intrinsic_inline_size_measurement_cache_get(
            self.callbacks.node_data(node),
            kind,
            key,
        )
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

    fn cache_intrinsic_inline_measurement(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        measurement: &MeasurementState,
        result: ChildLayoutResult,
        available_block_size: AvailableSize,
    ) {
        let used = measurement.root_used();
        self.intrinsic_inline_measurement_cache_put(
            node,
            kind,
            key,
            IntrinsicInlineSizeMeasurement {
                automatic_content_inline_size: result.automatic_content_inline_size,
                available_block_size,
                content_inline_size: used.content_inline_size.get(),
                content_block_size: used.content_block_size.get(),
                automatic_content_block_size: result.automatic_content_block_size,
                uses_collapsing_borders_model: used.uses_collapsing_borders_model.get(),
                has_first_baseline: used.has_first_baseline.get(),
                first_baseline: used.first_baseline.get(),
                has_last_baseline: used.has_last_baseline.get(),
                last_baseline: used.last_baseline.get(),
            },
        );
    }

    pub(crate) fn apply_cached_intrinsic_inline_measurement(
        &self,
        node: Node,
        available_inline_size: AvailableSize,
        available_block_size: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        // OPTIMIZATION: Calculating an intrinsic inline size already performs a complete measurement layout.
        // A later equivalent intrinsic line build only consumes the atomic box's measured dimensions and
        // baselines, so retain that summary instead of formatting the same descendants again. Commit layout
        // must still create all descendant geometry.
        if !self.state.is_measurement() {
            return None;
        }
        let kind = match available_inline_size {
            AvailableSize::MinContent => IntrinsicSizeCacheKind::MinContentInline,
            AvailableSize::MaxContent => IntrinsicSizeCacheKind::MaxContentInline,
            AvailableSize::Definite(_) | AvailableSize::Indefinite => return None,
        };
        let measurement = self.intrinsic_inline_measurement_cache_get(node, kind, cache_key(None, constraints))?;
        if measurement.available_block_size != available_block_size {
            return None;
        }
        let used = self.used_mut(node);
        if used.content_inline_size.get() != measurement.content_inline_size {
            return None;
        }

        used.set_content_block_size(measurement.content_block_size);
        used.uses_collapsing_borders_model
            .set(measurement.uses_collapsing_borders_model);
        used.has_first_baseline.set(measurement.has_first_baseline);
        used.first_baseline.set(measurement.first_baseline);
        used.has_last_baseline.set(measurement.has_last_baseline);
        used.last_baseline.set(measurement.last_baseline);
        Some(measurement.automatic_content_block_size)
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
                SizeDimension::Inline,
                ReplacedMaxContentSizeConstraints::default(),
            )
        {
            return fallback;
        }
        // Boxes with no children have zero intrinsic inline size.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        let key = cache_key(None, constraints);
        if let Some(cached) =
            self.intrinsic_inline_measurement_cache_get(node, IntrinsicSizeCacheKind::MinContentInline, key)
        {
            return cached.automatic_content_inline_size;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.inline_size_constraint.set(SizeConstraint::MinContent);
        root.has_definite_inline_size.set(false);
        let block_size = if root.has_definite_block_size() {
            AvailableSize::definite(root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        };
        let mut result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::MinContent,
                    block_size,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Root,
            },
        );
        result.automatic_content_inline_size = clamp_to_max_dimension_value(result.automatic_content_inline_size);
        let value = result.automatic_content_inline_size;
        self.cache_intrinsic_inline_measurement(
            node,
            IntrinsicSizeCacheKind::MinContentInline,
            key,
            &measurement,
            result,
            block_size,
        );
        value
    }

    pub(crate) fn calculate_max_content_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
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
            auto_size = ReplacedIntrinsicSize {
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
            |size: &ComputedSize, property: CyclicPercentageSizeProperty| -> Option<CssPixels> {
                if !size.is_length_percentage() {
                    return None;
                }
                match cyclic_percentage_intrinsic_contribution(
                    facts.is_replaced_box(),
                    size.contains_percentage(),
                    max_content_available,
                    property,
                ) {
                    CyclicPercentageIntrinsicContribution::TreatAsInitialValue => None,
                    CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                        let mut zero_constraints = constraints;
                        zero_constraints.percentage_basis_inline_size = Some(CssPixels::default());
                        Some(self.calculate_inner_inline_size(node, max_content_available, size, zero_constraints))
                    }
                    CyclicPercentageIntrinsicContribution::NotCyclic => {
                        if size.contains_percentage() && constraints.percentage_basis_inline_size.is_none() {
                            None
                        } else {
                            Some(self.calculate_inner_inline_size(node, max_content_available, size, constraints))
                        }
                    }
                }
            };
        let resolve_block_size = |size: &ComputedSize, property: CyclicPercentageSizeProperty| -> Option<CssPixels> {
            if !size.is_length_percentage() {
                return None;
            }
            if !size.contains_percentage() || constraints.percentage_basis_block_size.is_some() {
                return Some(self.calculate_inner_block_size(node, intrinsic_available_space, size, constraints));
            }
            match cyclic_percentage_intrinsic_contribution(
                facts.is_replaced_box(),
                size.contains_percentage(),
                max_content_available,
                property,
            ) {
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => None,
                CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    let mut zero_constraints = constraints;
                    zero_constraints.percentage_basis_block_size = Some(CssPixels::default());
                    Some(self.calculate_inner_block_size(node, intrinsic_available_space, size, zero_constraints))
                }
                CyclicPercentageIntrinsicContribution::NotCyclic => None,
            }
        };

        let definite_minimum_inline_size =
            resolve_destination_inline_size(style.min_width(), CyclicPercentageSizeProperty::MinSize);
        let definite_minimum_block_size = resolve_block_size(style.min_height(), CyclicPercentageSizeProperty::MinSize);
        let replaced_constraints = ReplacedMaxContentSizeConstraints {
            definite_size_in_ratio_determining_axis: definite_block_size,
            minimum_inline_size: definite_minimum_inline_size,
            minimum_block_size: definite_minimum_block_size,
        };
        if let Some(max_content_inline_size) = self.max_content_size_for_replaced_element_without_natural_size(
            node,
            auto_size,
            SizeDimension::Inline,
            replaced_constraints,
        ) {
            if definite_block_size.is_none()
                && facts.has_preferred_aspect_ratio()
                && let Some(definite_maximum_block_size) =
                    resolve_block_size(style.max_height(), CyclicPercentageSizeProperty::PreferredOrMaxSize)
            {
                // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
                // First, any definite minimum size is converted and transferred from the origin to destination axis.
                // This transferred minimum is capped by any definite preferred or maximum size in the destination axis.
                let mut transferred_minimum =
                    definite_minimum_block_size.map(|value| self.content_inline_size_from_aspect_ratio(node, value));
                if let Some(value) = transferred_minimum {
                    transferred_minimum = resolve_destination_inline_size(
                        style.width(),
                        CyclicPercentageSizeProperty::PreferredOrMaxSize,
                    )
                    .map_or(Some(value), |resolved| Some(value.min(resolved)));
                    let value = transferred_minimum.unwrap();
                    transferred_minimum = resolve_destination_inline_size(
                        style.max_width(),
                        CyclicPercentageSizeProperty::PreferredOrMaxSize,
                    )
                    .map_or(Some(value), |resolved| Some(value.min(resolved)));
                }

                // Then, any definite maximum size is converted and transferred from the origin to destination.
                // This transferred maximum is floored by any definite preferred or minimum size in the destination axis
                // as well as by the transferred minimum, if any.
                let mut transferred_maximum =
                    self.content_inline_size_from_aspect_ratio(node, definite_maximum_block_size);
                if let Some(resolved) =
                    resolve_destination_inline_size(style.width(), CyclicPercentageSizeProperty::PreferredOrMaxSize)
                {
                    transferred_maximum = transferred_maximum.max(resolved);
                }
                if let Some(resolved) =
                    resolve_destination_inline_size(style.min_width(), CyclicPercentageSizeProperty::MinSize)
                {
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
        let key = cache_key(None, constraints);
        if let Some(cached) =
            self.intrinsic_inline_measurement_cache_get(node, IntrinsicSizeCacheKind::MaxContentInline, key)
        {
            return cached.automatic_content_inline_size;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.inline_size_constraint.set(SizeConstraint::MaxContent);
        root.has_definite_inline_size.set(false);
        let block_size = if root.has_definite_block_size() {
            AvailableSize::definite(root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        };
        let mut result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::MaxContent,
                    block_size,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Root,
            },
        );
        result.automatic_content_inline_size = clamp_to_max_dimension_value(result.automatic_content_inline_size);
        let value = result.automatic_content_inline_size;
        self.cache_intrinsic_inline_measurement(
            node,
            IntrinsicSizeCacheKind::MaxContentInline,
            key,
            &measurement,
            result,
            block_size,
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
        let key = cache_key(Some(inline_size), constraints);
        if let Some(cached) = self.intrinsic_block_cache_get(node, IntrinsicSizeCacheKind::MinContentBlock, key) {
            return cached;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.block_size_constraint.set(SizeConstraint::MinContent);
        root.has_definite_block_size.set(false);
        root.set_content_inline_size(inline_size);
        let result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(inline_size),
                    block_size: AvailableSize::MinContent,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Root,
            },
        );
        let value = clamp_to_max_dimension_value(result.automatic_content_block_size);
        self.intrinsic_block_cache_put(node, IntrinsicSizeCacheKind::MinContentBlock, key, value);
        value
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
            SizeDimension::Block,
            ReplacedMaxContentSizeConstraints::default(),
        ) {
            return fallback;
        }
        // Boxes with no children have zero intrinsic height.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        let key = cache_key(Some(inline_size), constraints);
        if let Some(cached) = self.intrinsic_block_cache_get(node, IntrinsicSizeCacheKind::MaxContentBlock, key) {
            return cached;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.block_size_constraint.set(SizeConstraint::MaxContent);
        root.has_definite_block_size.set(false);
        root.set_content_inline_size(inline_size);
        let result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(inline_size),
                    block_size: AvailableSize::MaxContent,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                sizing: RootSizingDirectives::default(),
                participation: ParticipationInParentFormattingContext::Root,
            },
        );
        let value = clamp_to_max_dimension_value(result.automatic_content_block_size);
        self.intrinsic_block_cache_put(node, IntrinsicSizeCacheKind::MaxContentBlock, key, value);
        value
    }

    pub(crate) fn measure_automatic_content_block_size(
        &self,
        node: Node,
        layout_mode: LayoutMode,
        inner_available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        measurement
            .run_with_layout_mode(
                node,
                layout_mode,
                LayoutInput::new(inner_available_space, constraints, ParticipationInParentFormattingContext::Root),
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
        let used = self.used_mut(node);
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

    fn create_measurement_used_values(
        measurement: &MeasurementState,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &UsedValues {
        let callbacks = *measurement.callbacks();
        measurement
            .rust_state()
            .create_used_values(&callbacks, node, constraints)
    }

    // 17.5.2 Table width algorithms: the 'table-layout' property
    // https://www.w3.org/TR/CSS22/tables.html#width-layout
    pub(crate) fn compute_table_box_inline_size_inside_wrapper(
        &self,
        wrapper: Node,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
        table_wrapper_containing_block_inline_size: Option<CssPixels>,
        table_wrapper_inline_size_mode: TableWrapperInlineSizeMode,
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

        let measurement = MeasurementState::create(self.callbacks, wrapper, table_wrapper_constraints);

        // The table wrapper is invisible to percentage resolution, so the table box gets the
        // wrapper's constraints unchanged. Callers measuring a table wrapper for grid alignment
        // pass the grid-area inline size as the wrapper's percentage basis.
        let table_constraints = table_wrapper_constraints;
        let table_used = Self::create_measurement_used_values(&measurement, table_box, table_constraints);
        let table_style = self.style(table_box);
        table_used.border_left.set(table_style.border_left_width());
        table_used.border_right.set(table_style.border_right_width());
        table_used
            .padding_left
            .set(table_style.padding_left().to_px(containing_block_inline_size));
        table_used
            .padding_right
            .set(table_style.padding_right().to_px(containing_block_inline_size));

        let table_run = crate::layout::FormattingContextRun::new(
            measurement.rust_state(),
            table_box,
            LayoutMode::IntrinsicSizing,
            *measurement.callbacks(),
            false,
            false,
        );
        let mut table = TableFormattingContext::new(&table_run);
        let table_available = table_used.available_inner_space_or_constraints_from(available_space);
        table.run_until_inline_size_calculation(
            LayoutInput::new(table_available, table_constraints, ParticipationInParentFormattingContext::Root),
            true,
        );

        let table_used_inline_size = table_used.border_box_inline_size(false);
        if table_wrapper_inline_size_mode == TableWrapperInlineSizeMode::UseTableUsedInlineSizeIfNotAuto
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
        let table_box = self.table_box_inside_wrapper(wrapper);

        let measurement = MeasurementState::create(self.callbacks, wrapper, table_wrapper_constraints);
        measurement.run_with_layout_mode(
            wrapper,
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

        let table_used = measurement.rust_state().used_values(measurement.callbacks(), table_box);
        let table_used_block_size = table_used.border_box_block_size(table_used.uses_collapsing_borders_model.get());
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
            return subtract_border_box_adjustment(
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
            return subtract_border_box_adjustment(
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

    pub(crate) fn calculate_inner_inline_width(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.calculate_inner_inline_size(node, available, self.style(node).width(), constraints)
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
