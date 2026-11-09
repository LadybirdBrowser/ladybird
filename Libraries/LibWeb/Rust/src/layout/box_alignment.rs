/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://drafts.csswg.org/css-align-3/
//! The keywords of the self-alignment and content-distribution properties, resolved to the side of an axis that they
//! align to. Each layout mode sizes and places its boxes from the resolved alignment.

use super::*;

/// The low side of an axis, its center, or its high side. The low side is the left or top side in the frame the caller
/// measures offsets in, once the writing mode, the direction and any reversal of the axis are resolved.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AxisEdge {
    Start,
    Center,
    End,
}

impl AxisEdge {
    pub(crate) fn low_side_if(is_low: bool) -> Self {
        if is_low { Self::Start } else { Self::End }
    }

    pub(crate) fn reversed(self) -> Self {
        match self {
            Self::Start => Self::End,
            Self::Center => Self::Center,
            Self::End => Self::Start,
        }
    }

    /// The offset from the low side of an alignment container of a subject aligned to this edge, where the container
    /// leaves `free_space` beside the subject.
    pub(crate) fn offset(self, free_space: CssPixels) -> CssPixels {
        match self {
            Self::Start => CssPixels::default(),
            Self::Center => free_space / 2,
            Self::End => free_space,
        }
    }
}

/// https://drafts.csswg.org/css-align-3/#overflow-values
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum OverflowSafety {
    /// Neither safe nor unsafe is specified, so the layout mode decides.
    Default,
    Safe,
    Unsafe,
}

impl OverflowSafety {
    fn of(computed: u8) -> Self {
        match computed {
            OVERFLOW_ALIGNMENT_SAFE => Self::Safe,
            OVERFLOW_ALIGNMENT_UNSAFE => Self::Unsafe,
            _ => Self::Default,
        }
    }

    fn is_safe(self, layout_mode_default: Self) -> bool {
        match self {
            Self::Default => layout_mode_default == Self::Safe,
            safety => safety == Self::Safe,
        }
    }
}

/// The directions of an axis that the alignment keywords name its sides by, as whether each start side is its low side.
#[derive(Clone, Copy, Debug)]
pub(crate) struct AxisDirections {
    pub(crate) container_start_is_low: bool,
    /// The main-start or cross-start side of a flex container. Elsewhere, flex-start is the container's start side.
    pub(crate) flex_start_is_low: bool,
    /// The start side of the alignment subject, which self-start and self-end name.
    pub(crate) self_start_is_low: bool,
    /// The line-left or physical left side, or None where left and right behave as start.
    pub(crate) line_left_is_low: Option<bool>,
}

impl AxisDirections {
    pub(crate) fn of(container_start_is_low: bool, line_left_is_low: Option<bool>) -> Self {
        Self {
            container_start_is_low,
            flex_start_is_low: container_start_is_low,
            self_start_is_low: container_start_is_low,
            line_left_is_low,
        }
    }

    fn left_is_low(self) -> bool {
        self.line_left_is_low.unwrap_or(self.container_start_is_low)
    }

    fn right_is_low(self) -> bool {
        self.line_left_is_low
            .map_or(self.container_start_is_low, |left_is_low| !left_is_low)
    }
}

/// A keyword of justify-self, justify-items, align-self or align-items.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelfKeyword {
    Normal,
    Stretch,
    Baseline,
    Center,
    Start,
    End,
    SelfStart,
    SelfEnd,
    FlexStart,
    FlexEnd,
    Left,
    Right,
    AnchorCenter,
}

impl SelfKeyword {
    /// The justify-self keyword of a box styled `style` and its safety, where auto takes the justify-items value of
    /// the container styled `container_style`.
    pub(crate) fn justify_self_of(
        style: ComputedValuesView<'_>,
        container_style: ComputedValuesView<'_>,
    ) -> (Self, OverflowSafety) {
        let keyword = match style.justify_self() {
            justify_self::AUTO => {
                let keyword = match container_style.justify_items() {
                    justify_items::NORMAL | justify_items::LEGACY => Self::Normal,
                    justify_items::STRETCH => Self::Stretch,
                    justify_items::BASELINE => Self::Baseline,
                    justify_items::CENTER => Self::Center,
                    justify_items::START => Self::Start,
                    justify_items::END => Self::End,
                    justify_items::SELF_START => Self::SelfStart,
                    justify_items::SELF_END => Self::SelfEnd,
                    justify_items::FLEX_START => Self::FlexStart,
                    justify_items::FLEX_END => Self::FlexEnd,
                    justify_items::LEFT => Self::Left,
                    justify_items::RIGHT => Self::Right,
                    _ => unreachable!("invalid justify-items value"),
                };
                return (keyword, OverflowSafety::of(container_style.justify_items_overflow()));
            }
            justify_self::NORMAL => Self::Normal,
            justify_self::STRETCH => Self::Stretch,
            justify_self::BASELINE => Self::Baseline,
            justify_self::CENTER => Self::Center,
            justify_self::START => Self::Start,
            justify_self::END => Self::End,
            justify_self::SELF_START => Self::SelfStart,
            justify_self::SELF_END => Self::SelfEnd,
            justify_self::FLEX_START => Self::FlexStart,
            justify_self::FLEX_END => Self::FlexEnd,
            justify_self::LEFT => Self::Left,
            justify_self::RIGHT => Self::Right,
            justify_self::ANCHOR_CENTER => Self::AnchorCenter,
            _ => unreachable!("invalid justify-self value"),
        };
        (keyword, OverflowSafety::of(style.justify_self_overflow()))
    }

    /// The align-self keyword of a box styled `style` and its safety, where auto takes the align-items value of the
    /// container styled `container_style`.
    pub(crate) fn align_self_of(
        style: ComputedValuesView<'_>,
        container_style: ComputedValuesView<'_>,
    ) -> (Self, OverflowSafety) {
        let keyword = match style.align_self() {
            align_self::AUTO => {
                let keyword = match container_style.align_items() {
                    align_items::NORMAL => Self::Normal,
                    align_items::STRETCH => Self::Stretch,
                    align_items::BASELINE => Self::Baseline,
                    align_items::CENTER => Self::Center,
                    align_items::START => Self::Start,
                    align_items::END => Self::End,
                    align_items::SELF_START => Self::SelfStart,
                    align_items::SELF_END => Self::SelfEnd,
                    align_items::FLEX_START => Self::FlexStart,
                    align_items::FLEX_END => Self::FlexEnd,
                    _ => unreachable!("invalid align-items value"),
                };
                return (keyword, OverflowSafety::of(container_style.align_items_overflow()));
            }
            align_self::NORMAL => Self::Normal,
            align_self::STRETCH => Self::Stretch,
            align_self::BASELINE => Self::Baseline,
            align_self::CENTER => Self::Center,
            align_self::START => Self::Start,
            align_self::END => Self::End,
            align_self::SELF_START => Self::SelfStart,
            align_self::SELF_END => Self::SelfEnd,
            align_self::FLEX_START => Self::FlexStart,
            align_self::FLEX_END => Self::FlexEnd,
            align_self::ANCHOR_CENTER => Self::AnchorCenter,
            _ => unreachable!("invalid align-self value"),
        };
        (keyword, OverflowSafety::of(style.align_self_overflow()))
    }

    /// The alignment of a subject along an axis with the directions `directions`.
    /// https://drafts.csswg.org/css-anchor-position-1/#valdef-justify-self-anchor-center
    /// If the box is not absolutely positioned, or does not have a default anchor box, this value behaves as center
    /// and has no additional effect on how inset properties resolve.
    pub(crate) fn resolve(self, safety: OverflowSafety, directions: AxisDirections) -> SelfAlignment {
        let edge = |side_is_low| SelfPosition::Edge(AxisEdge::low_side_if(side_is_low));
        let position = match self {
            Self::Normal => SelfPosition::Normal,
            Self::Stretch => SelfPosition::Stretch,
            Self::Baseline => SelfPosition::Baseline,
            Self::Center | Self::AnchorCenter => SelfPosition::Edge(AxisEdge::Center),
            Self::Start => edge(directions.container_start_is_low),
            Self::End => edge(!directions.container_start_is_low),
            Self::SelfStart => edge(directions.self_start_is_low),
            Self::SelfEnd => edge(!directions.self_start_is_low),
            Self::FlexStart => edge(directions.flex_start_is_low),
            Self::FlexEnd => edge(!directions.flex_start_is_low),
            Self::Left => edge(directions.left_is_low()),
            Self::Right => edge(directions.right_is_low()),
        };
        SelfAlignment {
            position,
            safety,
            container_start: AxisEdge::low_side_if(directions.container_start_is_low),
        }
    }
}

/// Where a box aligns itself along an axis. The layout mode decides what normal alignment does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelfPosition {
    Normal,
    Stretch,
    Baseline,
    Edge(AxisEdge),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct SelfAlignment {
    pub(crate) position: SelfPosition,
    pub(crate) safety: OverflowSafety,
    /// The edge of the alignment container's start side.
    pub(crate) container_start: AxisEdge,
}

impl SelfAlignment {
    /// The position of a subject whose alignment container leaves `free_space` beside it, in a layout mode whose
    /// overflow alignment is `layout_mode_default` where the style specifies neither safe nor unsafe.
    /// https://drafts.csswg.org/css-align-3/#overflow-values
    /// safe: If the size of the alignment subject overflows the alignment container, the alignment subject is instead
    ///       aligned as if the alignment mode were start.
    pub(crate) fn position_for(self, free_space: CssPixels, layout_mode_default: OverflowSafety) -> SelfPosition {
        if free_space < CssPixels::default() && self.safety.is_safe(layout_mode_default) {
            return SelfPosition::Edge(self.container_start);
        }
        self.position
    }
}

/// A keyword of justify-content or align-content.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ContentKeyword {
    Normal,
    Stretch,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
    Center,
    Start,
    End,
    FlexStart,
    FlexEnd,
    Left,
    Right,
}

impl ContentKeyword {
    pub(crate) fn justify_content_of(style: ComputedValuesView<'_>) -> (Self, OverflowSafety) {
        let keyword = match style.justify_content() {
            justify_content::NORMAL => Self::Normal,
            justify_content::STRETCH => Self::Stretch,
            justify_content::SPACE_BETWEEN => Self::SpaceBetween,
            justify_content::SPACE_AROUND => Self::SpaceAround,
            justify_content::SPACE_EVENLY => Self::SpaceEvenly,
            justify_content::CENTER => Self::Center,
            justify_content::START => Self::Start,
            justify_content::END => Self::End,
            justify_content::FLEX_START => Self::FlexStart,
            justify_content::FLEX_END => Self::FlexEnd,
            justify_content::LEFT => Self::Left,
            justify_content::RIGHT => Self::Right,
            _ => unreachable!("invalid justify-content value"),
        };
        (keyword, OverflowSafety::Default)
    }

    /// The align-content keyword of a container styled `style` and its safety.
    /// AD-HOC: Baseline values align as flex-start rather than with their fallback alignment, matching other engines.
    pub(crate) fn align_content_of(style: ComputedValuesView<'_>) -> (Self, OverflowSafety) {
        let keyword = match style.align_content() {
            align_content::NORMAL => Self::Normal,
            align_content::STRETCH => Self::Stretch,
            align_content::SPACE_BETWEEN => Self::SpaceBetween,
            align_content::SPACE_AROUND => Self::SpaceAround,
            align_content::SPACE_EVENLY => Self::SpaceEvenly,
            align_content::CENTER => Self::Center,
            align_content::START => Self::Start,
            align_content::END => Self::End,
            align_content::FLEX_START | align_content::BASELINE => Self::FlexStart,
            align_content::FLEX_END => Self::FlexEnd,
            _ => unreachable!("invalid align-content value"),
        };
        (keyword, OverflowSafety::of(style.align_content_overflow()))
    }

    /// The alignment of the content of a container along an axis with the directions `directions`.
    /// https://drafts.csswg.org/css-align-3/#distribution-values
    pub(crate) fn resolve(self, safety: OverflowSafety, directions: AxisDirections) -> ContentAlignment {
        let edge = AxisEdge::low_side_if;
        let flex_start = edge(directions.flex_start_is_low);
        let (distribution, packed_against) = match self {
            // normal: [...] behaves as stretch.
            // stretch: The default fallback alignment for this value is flex-start.
            Self::Normal | Self::Stretch => (Some(ContentDistribution::Stretch), flex_start),
            // space-between: The default fallback alignment for this value is flex-start.
            Self::SpaceBetween => (Some(ContentDistribution::SpaceBetween), flex_start),
            // space-around: The default fallback alignment for this value is safe center.
            Self::SpaceAround => (Some(ContentDistribution::SpaceAround), AxisEdge::Center),
            // space-evenly: The default fallback alignment for this value is safe center.
            Self::SpaceEvenly => (Some(ContentDistribution::SpaceEvenly), AxisEdge::Center),
            Self::Center => (None, AxisEdge::Center),
            Self::Start => (None, edge(directions.container_start_is_low)),
            Self::End => (None, edge(!directions.container_start_is_low)),
            Self::FlexStart => (None, flex_start),
            Self::FlexEnd => (None, flex_start.reversed()),
            Self::Left => (None, edge(directions.left_is_low())),
            Self::Right => (None, edge(directions.right_is_low())),
        };
        ContentAlignment {
            distribution,
            edge: packed_against,
            safety,
            container_start: edge(directions.container_start_is_low),
        }
    }
}

/// https://drafts.csswg.org/css-align-3/#distribution-values
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ContentDistribution {
    Stretch,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
}

/// How a container aligns its content along an axis.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ContentAlignment {
    /// How free space is distributed between the alignment subjects, if it is.
    pub(crate) distribution: Option<ContentDistribution>,
    /// The edge the alignment subjects are packed against: their position, or the fallback alignment of their
    /// distribution.
    pub(crate) edge: AxisEdge,
    pub(crate) safety: OverflowSafety,
    /// The edge of the alignment container's start side.
    pub(crate) container_start: AxisEdge,
}

impl ContentAlignment {
    /// The offset from the low side of the alignment container of its content, as one subject that leaves
    /// `free_space` in the container, in a layout mode whose overflow alignment is `layout_mode_default` where the
    /// style specifies neither safe nor unsafe. The fallback alignments of space-around and space-evenly are safe.
    pub(crate) fn offset(self, free_space: CssPixels, layout_mode_default: OverflowSafety) -> CssPixels {
        let falls_back_safely = matches!(
            self.distribution,
            Some(ContentDistribution::SpaceAround | ContentDistribution::SpaceEvenly)
        ) || self.safety.is_safe(layout_mode_default);
        if free_space < CssPixels::default() && falls_back_safely {
            return self.container_start.offset(free_space);
        }
        self.edge.offset(free_space)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn px(value: i64) -> CssPixels {
        CssPixels::from_integer(value)
    }

    fn reversed_axis_with_left_low() -> AxisDirections {
        AxisDirections {
            flex_start_is_low: true,
            self_start_is_low: true,
            ..AxisDirections::of(false, Some(true))
        }
    }

    #[test]
    fn self_keywords_name_the_sides_of_their_directions() {
        let directions = reversed_axis_with_left_low();
        let edge_of = |keyword: SelfKeyword| keyword.resolve(OverflowSafety::Default, directions).position;
        assert_eq!(edge_of(SelfKeyword::Start), SelfPosition::Edge(AxisEdge::End));
        assert_eq!(edge_of(SelfKeyword::End), SelfPosition::Edge(AxisEdge::Start));
        assert_eq!(edge_of(SelfKeyword::FlexStart), SelfPosition::Edge(AxisEdge::Start));
        assert_eq!(edge_of(SelfKeyword::SelfEnd), SelfPosition::Edge(AxisEdge::End));
        assert_eq!(edge_of(SelfKeyword::Left), SelfPosition::Edge(AxisEdge::Start));
        assert_eq!(edge_of(SelfKeyword::Right), SelfPosition::Edge(AxisEdge::End));
        assert_eq!(edge_of(SelfKeyword::AnchorCenter), SelfPosition::Edge(AxisEdge::Center));
        assert_eq!(edge_of(SelfKeyword::Normal), SelfPosition::Normal);
    }

    #[test]
    fn left_and_right_behave_as_start_off_the_line_axis() {
        let directions = AxisDirections::of(false, None);
        for keyword in [SelfKeyword::Left, SelfKeyword::Right] {
            assert_eq!(
                keyword.resolve(OverflowSafety::Default, directions).position,
                SelfPosition::Edge(AxisEdge::End)
            );
        }
    }

    #[test]
    fn safe_self_alignment_falls_back_to_the_container_start_on_overflow() {
        let directions = reversed_axis_with_left_low();
        let safe = SelfKeyword::FlexStart.resolve(OverflowSafety::Safe, directions);
        assert_eq!(
            safe.position_for(px(10), OverflowSafety::Unsafe),
            SelfPosition::Edge(AxisEdge::Start)
        );
        assert_eq!(
            safe.position_for(px(-10), OverflowSafety::Unsafe),
            SelfPosition::Edge(AxisEdge::End)
        );
        let default = SelfKeyword::FlexStart.resolve(OverflowSafety::Default, directions);
        assert_eq!(
            default.position_for(px(-10), OverflowSafety::Unsafe),
            SelfPosition::Edge(AxisEdge::Start)
        );
        assert_eq!(
            default.position_for(px(-10), OverflowSafety::Safe),
            SelfPosition::Edge(AxisEdge::End)
        );
    }

    #[test]
    fn content_offsets_follow_the_edge_and_the_distribution_fallbacks() {
        let directions = AxisDirections::of(true, None);
        let offset = |keyword: ContentKeyword, free_space: i64| {
            keyword
                .resolve(OverflowSafety::Default, directions)
                .offset(px(free_space), OverflowSafety::Unsafe)
        };
        assert_eq!(offset(ContentKeyword::Center, -20), px(-10));
        assert_eq!(offset(ContentKeyword::End, 20), px(20));
        assert_eq!(offset(ContentKeyword::SpaceAround, 20), px(10));
        assert_eq!(offset(ContentKeyword::SpaceEvenly, -20), px(0));
        assert_eq!(offset(ContentKeyword::SpaceBetween, 20), px(0));
        assert_eq!(offset(ContentKeyword::Normal, 20), px(0));
    }
}
