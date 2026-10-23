/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// https://w3c.github.io/mathml-core/#layout-algorithms

use super::*;

/// The layout algorithm a MathML element's tag name selects when its computed display is block math or inline math.
/// Mirrors the C++ `MathMLLayoutKind`, which the element's construction facts carry.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum MathLayoutKind {
    #[default]
    NotMathML,
    Mrow,
    Math,
    Token,
    Mo,
    Mspace,
    Mfrac,
    Msqrt,
    Mroot,
    Msub,
    Msup,
    Msubsup,
    Munder,
    Mover,
    Munderover,
    Mmultiscripts,
    Mprescripts,
    None,
    Mpadded,
}

impl MathLayoutKind {
    pub(crate) fn from_construction_facts(facts: u32) -> Self {
        use crate::css::style::bridge::element_construction_fact::{MATHML_LAYOUT_KIND_MASK, MATHML_LAYOUT_KIND_SHIFT};
        match (facts & MATHML_LAYOUT_KIND_MASK) >> MATHML_LAYOUT_KIND_SHIFT {
            1 => Self::Mrow,
            2 => Self::Math,
            3 => Self::Token,
            4 => Self::Mo,
            5 => Self::Mspace,
            6 => Self::Mfrac,
            7 => Self::Msqrt,
            8 => Self::Mroot,
            9 => Self::Msub,
            10 => Self::Msup,
            11 => Self::Msubsup,
            12 => Self::Munder,
            13 => Self::Mover,
            14 => Self::Munderover,
            15 => Self::Mmultiscripts,
            16 => Self::Mprescripts,
            17 => Self::None,
            18 => Self::Mpadded,
            _ => Self::NotMathML,
        }
    }

    /// Whether boxes of this kind lay out their children with a MathML layout algorithm. Token elements lay out their
    /// text as a block box.
    pub(crate) fn uses_math_layout(self) -> bool {
        !matches!(self, Self::NotMathML | Self::Token | Self::Mo)
    }
}

/// An in-flow child box after its layout. The ascent and descent are measured from the alphabetic baseline to the
/// line-over and line-under edges of the child's margin box.
#[derive(Clone, Copy)]
struct Child {
    node: Node,
    inline_size: CssPixels,
    ascent: CssPixels,
    descent: CssPixels,
}

/// Where a child's margin box goes: its line-left offset, and how far its alphabetic baseline is shifted from the
/// alphabetic baseline of the math content towards the line-under.
#[derive(Clone, Copy, Default)]
struct ChildPosition {
    line_left_offset: CssPixels,
    baseline_shift: CssPixels,
}

/// The metrics of a math content box and the positions of its in-flow children.
#[derive(Default)]
struct MathContent {
    inline_size: CssPixels,
    ascent: CssPixels,
    descent: CssPixels,
    positions: Vec<ChildPosition>,
}

/// Font parameters used by math layout. Without an OpenType MATH table, these are the fallback values described in
/// https://w3c.github.io/mathml-core/#layout-constants-mathconstants
struct MathConstants {
    axis_height: CssPixels,
    default_rule_thickness: CssPixels,
    x_height: CssPixels,
    em: CssPixels,
}

impl MathConstants {
    fn for_style(style: &StyleValues<'_>) -> Self {
        let em = style.font_size();
        let x_height = CssPixels::nearest_value_for_f32(style.font_x_height());
        Self {
            // axisHeight: MATH.MathConstants.axisHeight or half OS/2.sxHeight if the constant is not available.
            axis_height: x_height / 2,
            // Default rule thickness: post.underlineThickness or Default fallback constant if the constant is not
            // available.
            // FIXME: Use post.underlineThickness of the first available font.
            // AD-HOC: Until then, use 0.05em, a typical underline thickness, rather than the Default fallback constant
            //         of 0, which would make every gap based on the rule thickness disappear.
            default_rule_thickness: em.scaled(0.05),
            x_height,
            em,
        }
    }
}

pub(super) struct MathFormattingContext<'pass> {
    run: FormattingContextRun<'pass>,
    content: MathContent,
    derived_baselines_of_root_box: DerivedBaselines,
}

impl<'pass> MathFormattingContext<'pass> {
    pub(super) fn new(run: &FormattingContextRun<'pass>) -> Self {
        Self {
            run: FormattingContextRun {
                previous_line_data: None,
                ..run.clone()
            },
            content: MathContent::default(),
            derived_baselines_of_root_box: DerivedBaselines::default(),
        }
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        StyleValues::for_node(&self.run.callbacks, node)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.run.callbacks, node)
    }

    fn kind(&self, node: Node) -> MathLayoutKind {
        self.run.callbacks.arena().math_layout_kind(node)
    }

    fn in_flow_children(&self) -> Vec<Node> {
        // https://w3c.github.io/mathml-core/#css-styling
        // The float property does not create floating of elements whose parent's computed display value is block
        // math or inline math, and does not take them out-of-flow.
        self.run
            .callbacks
            .children(self.run.box_)
            .filter(|&child| {
                let facts = self.facts(child);
                facts.is_box()
                    && !facts.is_absolutely_positioned()
                    && !self.run.callbacks.can_skip_is_anonymous_text_run(child)
            })
            .collect()
    }

    fn layout_child(&self, child: Node, constraints: ContainingBlockConstraints) -> Child {
        let run = &self.run;
        run.records.create_used_values(&run.callbacks, child, constraints);
        // NB: MathML layout does not break lines, so each child is laid out at its max-content inline size, as an
        //     atomic box whose block size follows from its content.
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: AvailableSize::Indefinite,
            },
            constraints,
            ParticipationInParentFormattingContext::AtomicInline,
        );
        let used = run.records.used_values(child);
        let baselines =
            match formatting_context::layout_inside_child(run, None, None, child, run.layout_mode, input, true) {
                ChildLayoutOutcome::Created(result) => result.baselines,
                ChildLayoutOutcome::ReenterCurrent | ChildLayoutOutcome::Skipped => used.content_baselines_from_cells(),
            };
        let collapsed = used.uses_collapsing_borders_model.get();
        let block_size = used.margin_box_block_size(collapsed);
        // NB: A box without a baseline is aligned by its line-under margin edge.
        let ascent = baselines
            .first
            .map_or(block_size, |baseline| used.margin_box_top(collapsed) + baseline);
        Child {
            node: child,
            inline_size: used.margin_box_inline_size(collapsed),
            ascent,
            descent: block_size - ascent,
        }
    }

    pub(super) fn run(&mut self, run: &FormattingContextRun<'pass>, layout_input: LayoutInput) {
        let root = run.box_;
        let kind = self.kind(root);
        let constraints = run
            .sizing()
            .constraints_for_child_context(root, layout_input.containing_block_constraints);
        let children: Vec<Child> = self
            .in_flow_children()
            .into_iter()
            .map(|child| self.layout_child(child, constraints))
            .collect();

        let root_style = self.style(root);
        let constants = MathConstants::for_style(&root_style);
        let content = match (kind, children.len()) {
            (MathLayoutKind::Mfrac, 2) => self.layout_fraction(&children, &constants, &root_style),
            (MathLayoutKind::Msub, 2) => layout_scripts(&children[0], Some(&children[1]), None, &constants),
            (MathLayoutKind::Msup, 2) => layout_scripts(&children[0], None, Some(&children[1]), &constants),
            (MathLayoutKind::Msubsup, 3) => {
                layout_scripts(&children[0], Some(&children[1]), Some(&children[2]), &constants)
            }
            (MathLayoutKind::Munder, 2) => layout_underover(&children[0], Some(&children[1]), None, &constants),
            (MathLayoutKind::Mover, 2) => layout_underover(&children[0], None, Some(&children[1]), &constants),
            (MathLayoutKind::Munderover, 3) => {
                layout_underover(&children[0], Some(&children[1]), Some(&children[2]), &constants)
            }
            _ => layout_row(&children),
        };

        // https://w3c.github.io/mathml-core/#layout-algorithms
        // If a different size is specified for the content box, that size is used instead and the inline-start and
        // block-start edges of the math content box are aligned with the ones of the content box, unless otherwise
        // specified in a specific layout algorithm.
        let root_used = run.records.used_values(root);
        let content_box_inline_size = root_used.content_inline_size.get();
        let centers_math_content = match kind {
            // If the computed display property is equal to block math then that math content box is rendered
            // horizontally centered within the content box.
            MathLayoutKind::Math => self.facts(root).display().is_block_outside(),
            // The math content box is placed within the content box so that their block-start edges are aligned and
            // the middles of these edges are at the same position.
            MathLayoutKind::Mfrac | MathLayoutKind::Munder | MathLayoutKind::Mover | MathLayoutKind::Munderover => true,
            _ => false,
        };
        let content_box_is_known = run.layout_mode == LayoutMode::Normal && root_used.has_definite_inline_size();
        let is_rtl = root_style.direction() == direction::RTL;
        let mut math_content_line_left = CssPixels::default();
        if content_box_is_known {
            if centers_math_content {
                math_content_line_left = (content_box_inline_size - content.inline_size) / 2;
            } else if is_rtl {
                math_content_line_left = content_box_inline_size - content.inline_size;
            }
        }

        for (child, position) in children.iter().zip(content.positions.iter()) {
            let used = run.records.used_values(child.node);
            let collapsed = used.uses_collapsing_borders_model.get();
            let line_left_offset = if is_rtl {
                content.inline_size - position.line_left_offset - child.inline_size
            } else {
                position.line_left_offset
            };
            let margin_box_top = content.ascent + position.baseline_shift - child.ascent;
            let offset = FfiCssPixelPoint {
                x: math_content_line_left + line_left_offset + used.margin_left.get() + used.border_box_left(collapsed),
                y: margin_box_top + used.margin_box_top(collapsed),
            };
            formatting_context::place_child(run, child.node, offset, None);
        }

        self.derived_baselines_of_root_box = DerivedBaselines {
            first: Some(content.ascent),
            last: Some(content.ascent),
        };
        self.content = content;
    }

    // https://w3c.github.io/mathml-core/#fraction-with-nonzero-line-thickness
    fn layout_fraction(&self, children: &[Child], constants: &MathConstants, style: &StyleValues<'_>) -> MathContent {
        let numerator = &children[0];
        let denominator = &children[1];
        let is_display_style = style.math_style() == math_style::NORMAL;
        // FIXME: Support the linethickness attribute.
        let line_thickness = constants.default_rule_thickness;
        let half_thickness = line_thickness / 2;
        let axis_height = constants.axis_height;
        // NB: FractionNumeratorShiftUp and FractionDenominatorShiftDown fall back to 0.
        let (numerator_gap_min, denominator_gap_min) = if is_display_style {
            (
                constants.default_rule_thickness * 3usize,
                constants.default_rule_thickness * 3usize,
            )
        } else {
            (constants.default_rule_thickness, constants.default_rule_thickness)
        };

        // The inline size of the math content is the maximum between the inline size of the numerator's margin box
        // and the inline size of the denominator's margin box.
        let inline_size = numerator.inline_size.max(denominator.inline_size);

        // NumeratorShift is the maximum between FractionNumeratorShiftUp and the AxisHeight + half the fraction line
        // thickness + FractionNumeratorGapMin + the ink line-descent of the numerator's margin box.
        let numerator_shift =
            CssPixels::default().max(axis_height + half_thickness + numerator_gap_min + numerator.descent);
        // DenominatorShift is the maximum between FractionDenominatorShiftDown and half the fraction line thickness +
        // FractionDenominatorGapMin + the ink line-ascent of the denominator's margin box − the AxisHeight.
        let denominator_shift =
            CssPixels::default().max(half_thickness + denominator_gap_min + denominator.ascent - axis_height);

        let ascent = (numerator_shift + numerator.ascent)
            .max(denominator.ascent - denominator_shift)
            .max(axis_height + half_thickness);
        let descent = (numerator.descent - numerator_shift)
            .max(denominator_shift + denominator.descent)
            .max(half_thickness - axis_height)
            .max(CssPixels::default());

        MathContent {
            inline_size,
            ascent,
            descent,
            positions: vec![
                ChildPosition {
                    line_left_offset: (inline_size - numerator.inline_size) / 2,
                    baseline_shift: -numerator_shift,
                },
                ChildPosition {
                    line_left_offset: (inline_size - denominator.inline_size) / 2,
                    baseline_shift: denominator_shift,
                },
            ],
        }
    }

    pub(super) fn parent_did_dimension(&self) {
        if self.run.layout_mode != LayoutMode::Normal {
            return;
        }
        let root_used = self.run.records.used_values(self.run.box_);
        for child in self.run.callbacks.children(self.run.box_) {
            let facts = self.facts(child);
            if !facts.is_box() || !facts.is_absolutely_positioned() {
                continue;
            }
            let static_position_rect = abspos_inputs::StaticPositionRect {
                rect: geometry::LogicalRect {
                    offset: Default::default(),
                    size: geometry::LogicalSize {
                        inline_size: root_used.content_inline_size.get(),
                        block_size: root_used.content_block_size.get(),
                    },
                },
                inline_alignment: abspos_inputs::StaticPositionAlignment::Start,
                block_alignment: abspos_inputs::StaticPositionAlignment::Start,
                alignment_derives_from_own_computed_values: false,
                is_known: true,
            };
            formatting_context::register_contained_abspos_child(
                &self.run.callbacks,
                self.run.fragments.as_deref(),
                self.run.box_,
                child,
                static_position_rect,
                None,
            );
        }
    }

    pub(super) fn derived_baselines_of_root_box(&self) -> DerivedBaselines {
        self.derived_baselines_of_root_box
    }

    pub(super) fn automatic_content_inline_size(&self) -> CssPixels {
        self.content.inline_size
    }

    pub(super) fn automatic_content_block_size(&self) -> CssPixels {
        self.content.ascent + self.content.descent
    }
}

// https://w3c.github.io/mathml-core/#layout-of-mrow
fn layout_row(children: &[Child]) -> MathContent {
    // FIXME: Add the lspace and rspace of embellished operators, and handle italic correction.
    let mut inline_offset = CssPixels::default();
    let mut ascent = CssPixels::default();
    let mut descent = CssPixels::default();
    let mut positions = Vec::with_capacity(children.len());
    for child in children {
        // Set the inline offset of the child to inline-offset and its block offset such that the alphabetic baseline
        // of the child is aligned with the alphabetic baseline.
        positions.push(ChildPosition {
            line_left_offset: inline_offset,
            baseline_shift: CssPixels::default(),
        });
        // Increment inline-offset by the inline size of the child's margin box.
        inline_offset += child.inline_size;
        // The line-ascent (respectively line-descent) of the math content is the maximum of the line-ascents
        // (respectively line-descents) of all the in-flow children's margin boxes.
        ascent = ascent.max(child.ascent);
        descent = descent.max(child.descent);
    }
    MathContent {
        inline_size: inline_offset,
        ascent,
        descent,
        positions,
    }
}

// https://w3c.github.io/mathml-core/#subscripts-and-superscripts-msub-msup-msubsup
fn layout_scripts(
    base: &Child,
    subscript: Option<&Child>,
    superscript: Option<&Child>,
    constants: &MathConstants,
) -> MathContent {
    // FIXME: Use the italic correction of the base and the MATH table constants of the font.
    // spaceAfterScript: MATH.MathConstants.spaceAfterScript or 1/24em if the constant is not available.
    let space_after_script = constants.em.scaled(1.0 / 24.0);
    // subscriptTopMax: ⅘ × OS/2.sxHeight if the constant is not available.
    let subscript_top_max = constants.x_height.scaled(0.8);
    // superscriptBottomMin: ¼ × OS/2.sxHeight if the constant is not available.
    let superscript_bottom_min = constants.x_height.scaled(0.25);
    // superscriptBottomMaxWithSubscript: ⅘ × OS/2.sxHeight if the constant is not available.
    let superscript_bottom_max_with_subscript = constants.x_height.scaled(0.8);
    // subSuperscriptGapMin: 4 × default rule thickness if the constant is not available.
    let sub_superscript_gap_min = constants.default_rule_thickness * 4usize;
    // FIXME: SubscriptShiftDown and SuperscriptShiftUp fall back to OS/2.ySubscriptYOffset and
    //        OS/2.ySuperscriptYOffset, which are not available, so they are 0 here.
    // NB: SuperscriptShiftUpCramped, SubscriptBaselineDropMin and SuperscriptBaselineDropMax fall back to the Default
    //     fallback constant, which is 0.

    // SubShift is the maximum between SubscriptShiftDown, the ink line-ascent of the subscript's margin box −
    // SubscriptTopMax, and SubscriptBaselineDropMin + the ink line-descent of the msub base's margin box.
    let mut sub_shift = subscript.map(|subscript| {
        CssPixels::default()
            .max(subscript.ascent - subscript_top_max)
            .max(base.descent)
    });
    // SuperShift is the maximum between SuperscriptShiftUp (or SuperscriptShiftUpCramped), SuperscriptBottomMin +
    // the ink line-descent of the superscript's margin box, and the ink line-ascent of the msup base's margin box −
    // SuperscriptBaselineDropMax.
    let mut super_shift = superscript.map(|superscript| {
        CssPixels::default()
            .max(superscript_bottom_min + superscript.descent)
            .max(base.ascent)
    });

    if let (Some(subscript), Some(superscript), Some(sub), Some(sup)) =
        (subscript, superscript, sub_shift.as_mut(), super_shift.as_mut())
    {
        // SubSuperGap is the gap between the two scripts along the block axis.
        let sub_super_gap = (*sub - subscript.ascent) + (*sup - superscript.descent);
        if sub_super_gap < sub_superscript_gap_min {
            // Let Δ be SuperscriptBottomMaxWithSubscript − (SuperShift − the ink line-descent of the msubsup
            // superscript's margin box). If Δ > 0 then set Δ to the minimum between Δ and SubSuperscriptGapMin −
            // SubSuperGap and increase SuperShift (and so SubSuperGap too) by Δ.
            let mut delta = superscript_bottom_max_with_subscript - (*sup - superscript.descent);
            let mut gap = sub_super_gap;
            if delta > CssPixels::default() {
                delta = delta.min(sub_superscript_gap_min - gap);
                *sup += delta;
                gap += delta;
            }
            // Let Δ be SubSuperscriptGapMin − SubSuperGap. If Δ > 0 then increase SubscriptShift by Δ.
            let delta = sub_superscript_gap_min - gap;
            if delta > CssPixels::default() {
                *sub += delta;
            }
        }
    }

    let mut ascent = base.ascent;
    let mut descent = base.descent;
    let mut positions = vec![ChildPosition::default()];
    let mut inline_size = base.inline_size;
    if let (Some(subscript), Some(sub)) = (subscript, sub_shift) {
        ascent = ascent.max(subscript.ascent - sub);
        descent = descent.max(subscript.descent + sub);
        positions.push(ChildPosition {
            line_left_offset: base.inline_size,
            baseline_shift: sub,
        });
        inline_size = inline_size.max(base.inline_size + subscript.inline_size + space_after_script);
    }
    if let (Some(superscript), Some(sup)) = (superscript, super_shift) {
        ascent = ascent.max(superscript.ascent + sup);
        descent = descent.max(superscript.descent - sup);
        positions.push(ChildPosition {
            line_left_offset: base.inline_size,
            baseline_shift: -sup,
        });
        inline_size = inline_size.max(base.inline_size + superscript.inline_size + space_after_script);
    }
    MathContent {
        inline_size,
        ascent,
        descent,
        positions,
    }
}

// https://w3c.github.io/mathml-core/#underscripts-and-overscripts-munder-mover-munderover
fn layout_underover(
    base: &Child,
    underscript: Option<&Child>,
    overscript: Option<&Child>,
    constants: &MathConstants,
) -> MathContent {
    // FIXME: Handle accents, large operators and operators stretchy along the inline axis.
    // underbarVerticalGap and overbarVerticalGap: 3 × default rule thickness if the constant is not available.
    let under_shift = constants.default_rule_thickness * 3usize;
    let over_shift = constants.default_rule_thickness * 3usize;
    // underbarExtraDescender and overbarExtraAscender: default rule thickness if the constant is not available.
    let under_extra_descender = constants.default_rule_thickness;
    let over_extra_ascender = constants.default_rule_thickness;

    // NB: The children are centered on the same vertical axis.
    let inline_size = [Some(base), underscript, overscript]
        .into_iter()
        .flatten()
        .map(|child| child.inline_size)
        .max()
        .unwrap_or_default();
    let centered = |child: &Child| (inline_size - child.inline_size) / 2;

    let mut ascent = base.ascent;
    let mut descent = base.descent;
    let mut positions = vec![ChildPosition {
        line_left_offset: centered(base),
        baseline_shift: CssPixels::default(),
    }];
    if let Some(underscript) = underscript {
        // AD-HOC: The underscript's margin box is placed UnderShift below the base's margin box, so that UnderShift is
        //         the gap between them, rather than placing the alphabetic baseline of the underscript UnderShift
        //         below the line-under edge of the base, which would make it overlap the base.
        let shift = base.descent + under_shift + underscript.ascent;
        descent = descent.max(shift + underscript.descent + under_extra_descender);
        positions.push(ChildPosition {
            line_left_offset: centered(underscript),
            baseline_shift: shift,
        });
    }
    if let Some(overscript) = overscript {
        // AD-HOC: The overscript's margin box is placed OverShift above the base's margin box, so that OverShift is
        //         the gap between them, rather than placing the alphabetic baseline of the overscript OverShift above
        //         the line-over edge of the base, which would make it overlap the base.
        let shift = base.ascent + over_shift + overscript.descent;
        ascent = ascent.max(shift + overscript.ascent + over_extra_ascender);
        positions.push(ChildPosition {
            line_left_offset: centered(overscript),
            baseline_shift: -shift,
        });
    }
    MathContent {
        inline_size,
        ascent,
        descent,
        positions,
    }
}
