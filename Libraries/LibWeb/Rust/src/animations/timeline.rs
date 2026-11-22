/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Timelines.
//! https://www.w3.org/TR/web-animations-1/#timelines

use super::animation::{
    AnimationHandle, AttachmentInterval, AttachmentRangeEdge, AttachmentRangeEdgeOffset, HostCellKind, WeakAnimationSet,
};
use super::snapshot::{TimelineClock, TimelineClockSource};
use super::timeline_state::TimelineState;
use super::timing::{TimeValue, TimelineSample};
use crate::css::css_enums::{direction, writing_mode};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::{RetainedStyleValueData, optional_style_values_equal};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_read::{GeometryRead, PaintRow};
use crate::painting::paintable_geometry::{
    first_fragment_border_box_rect, first_fragment_padding_box_rect, has_scrollable_overflow, scrollable_overflow_rect,
    unbroken_content_size,
};
use std::cell::{Cell, Ref, RefCell, RefMut};
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

/// The identity of a timeline, by which a clock tick finds the progress of the scroller a scroll timeline follows.
#[repr(transparent)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct AnimationTimelineId(pub u64);

impl AnimationTimelineId {
    fn mint() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self(NEXT.fetch_add(1, Ordering::Relaxed))
    }
}

/// The host's `TimelineRangeName`.
/// https://drafts.csswg.org/scroll-animations-1/#named-ranges
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TimelineRangeName {
    /// NB: An edge given without a range name is measured along the entire timeline.
    None,
    Cover,
    Contain,
    Entry,
    Exit,
    EntryCrossing,
    ExitCrossing,
    Scroll,
}

impl TimelineRangeName {
    fn from_host(name: u8) -> Self {
        match name {
            0 => Self::None,
            1 => Self::Cover,
            2 => Self::Contain,
            3 => Self::Entry,
            4 => Self::Exit,
            5 => Self::EntryCrossing,
            6 => Self::ExitCrossing,
            7 => Self::Scroll,
            _ => unreachable!("the host names a timeline range it knows"),
        }
    }
}

/// A length plus a percentage of what it resolves against, or a calculation only that resolves.
fn length_percentage_px(calculated: &RetainedStyleValueData, px: f64, percentage: f64, basis: f64) -> f64 {
    match calculated.optional_data() {
        Some(calculated) => crate::css::style_compute::length_percentage_px(calculated, basis),
        None => px + percentage / 100.0 * basis,
    }
}

impl AttachmentRangeEdgeOffset {
    /// The offset, in pixels, along a range of the given size.
    fn offset_along(&self, range_size: f64) -> f64 {
        length_percentage_px(&self.calculated, self.px, self.percentage, range_size)
    }
}

/// One side of the view progress visibility range: auto takes the scroll container's scroll padding, otherwise a
/// length plus a percentage of the scrollport.
/// https://drafts.csswg.org/scroll-animations-1/#view-timeline-inset
#[derive(Clone)]
pub(crate) struct ViewTimelineInsetEdge {
    pub(crate) is_auto: bool,
    pub(crate) px: f64,
    pub(crate) percentage: f64,
    /// NB: An inset only a calculation can describe, such as min(50%, 10px), resolves against the scrollport size.
    pub(crate) calculated: RetainedStyleValueData,
}

/// One side of the view progress visibility range that is auto, or a length plus a percentage of the scrollport, as a
/// clock tick resolves it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct LinearInsetEdge {
    is_auto: bool,
    px: f64,
    percentage: f64,
}

impl LinearInsetEdge {
    /// The inset, in pixels, in a scrollport of the given size, where auto takes `scroll_padding`.
    fn resolve(&self, scrollport_size: f64, scroll_padding: f64) -> f64 {
        if self.is_auto {
            return scroll_padding;
        }
        self.px + self.percentage / 100.0 * scrollport_size
    }
}

impl ViewTimelineInsetEdge {
    /// The inset, in pixels, in a scrollport of the given size, where auto takes `scroll_padding`.
    fn resolve(&self, scrollport_size: f64, scroll_padding: f64) -> f64 {
        if !self.is_auto
            && let Some(calculated) = self.calculated.optional_data()
        {
            return crate::css::style_compute::length_percentage_px(calculated, scrollport_size);
        }
        self.as_length_plus_percentage()
            .resolve(scrollport_size, scroll_padding)
    }

    fn as_length_plus_percentage(&self) -> LinearInsetEdge {
        LinearInsetEdge {
            is_auto: self.is_auto,
            px: self.px,
            percentage: self.percentage,
        }
    }

    /// The inset as a clock tick resolves it, where it is auto or a length plus a percentage, which not every
    /// calculation is.
    fn as_linear(&self) -> Option<LinearInsetEdge> {
        self.calculated
            .optional_data()
            .is_none_or(crate::css::style_compute::length_percentage_is_linear)
            .then(|| self.as_length_plus_percentage())
    }

    fn equals(&self, other: &Self) -> bool {
        self.is_auto == other.is_auto
            && self.px == other.px
            && self.percentage == other.percentage
            && optional_style_values_equal(self.calculated.optional_data(), other.calculated.optional_data())
    }
}

/// The start and the end inset of a view progress visibility range.
#[derive(Clone)]
pub(crate) struct ViewTimelineInset {
    pub(crate) start: ViewTimelineInsetEdge,
    pub(crate) end: ViewTimelineInsetEdge,
}

impl ViewTimelineInset {
    pub(crate) fn equals(&self, other: &Self) -> bool {
        self.start.equals(&other.start) && self.end.equals(&other.end)
    }
}

/// The scroll node of the host's `AsyncScrollNodeStableID`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScrollNodeStableId {
    pub node_id: i64,
    pub kind: u8,
    pub pseudo_element_type: u8,
}

/// The scroll node the timeline followed at its last update, the style node of the element whose box scrolls, or 0
/// for the viewport, and the scroll offset there and at 100% progress, in CSS pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct FollowedScroller {
    pub(crate) scroll_node: ScrollNodeStableId,
    pub(crate) scroller_style_node: u32,
    pub(crate) vertical: bool,
    pub(crate) scroll_offset: f64,
    pub(crate) max_scroll_offset: f64,
}

/// The scroll container a scroll timeline follows, along the timeline's axis, in CSS pixels, as the host laid it out.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ScrollerMetrics {
    pub(crate) scroll_node: Option<ScrollNodeStableId>,
    pub(crate) scroller_style_node: u32,
    pub(crate) is_vertical: bool,
    pub(crate) scroll_offset: f64,
    pub(crate) max_scroll_offset: f64,
    /// Where the scrollport starts in the coordinates the boxes inside it are laid out in, and its size.
    pub(crate) scrollport_start: f64,
    pub(crate) scrollport_size: f64,
    /// The scroll padding at the start and at the end of the scrollport.
    pub(crate) scroll_padding_start: f64,
    pub(crate) scroll_padding_end: f64,
}

/// The length along the vertical axis, where `vertical`, and along the horizontal one otherwise, of what `vertical` and
/// `horizontal` measure.
fn along(vertical: bool, vertical_length: CssPixels, horizontal_length: CssPixels) -> f64 {
    if vertical {
        vertical_length.to_double()
    } else {
        horizontal_length.to_double()
    }
}

impl ScrollerMetrics {
    /// The scroll container whose box `arena` lays out in `scroller`, along the vertical axis where `vertical`, and
    /// along the horizontal one otherwise, where its box has scrollable overflow. Its scroll padding is measured only
    /// where `with_scroll_padding`. `scroll_node` and `scroller_style_node` name it as `FollowedScroller` does.
    pub(crate) fn measure(
        arena: &LayoutNodeArena,
        scroller: NodeSlotId,
        scroll_node: Option<ScrollNodeStableId>,
        scroller_style_node: u32,
        vertical: bool,
        with_scroll_padding: bool,
    ) -> Option<Self> {
        if !has_scrollable_overflow(arena, scroller) {
            return None;
        }
        let scrollable_overflow_rect = scrollable_overflow_rect(arena, scroller)?;
        let content_size = unbroken_content_size(arena, scroller);
        let scrollport = first_fragment_padding_box_rect(arena, scroller);
        // NB: Only a view progress timeline measures its subject against the scroll padding, so a scroll progress
        //     timeline does not resolve it on every update.
        let snapport = if with_scroll_padding {
            crate::painting::scroll_snap::scroll_snapport_rect(arena, scroller, scrollport)
        } else {
            scrollport
        };

        // FIXME: Scroll offset is currently incorrect as it is always relative to the top left of the scrollable
        //        overflow rect when it should instead be relative to the scroll origin.

        // FIXME: Support the case where the computed scroll axis is reversed

        let scroll_offset = arena.row_scroll_offset(scroller);
        let bottom = |rect: CssPixelRect| rect.y + rect.height;
        let right = |rect: CssPixelRect| rect.x + rect.width;
        Some(Self {
            scroll_node,
            scroller_style_node,
            is_vertical: vertical,
            scroll_offset: along(vertical, scroll_offset.y, scroll_offset.x),
            max_scroll_offset: if vertical {
                scrollable_overflow_rect.height.to_double() - content_size.height.to_double()
            } else {
                scrollable_overflow_rect.width.to_double() - content_size.width.to_double()
            },
            scrollport_start: along(vertical, scrollport.y, scrollport.x),
            scrollport_size: along(vertical, scrollport.height, scrollport.width),
            scroll_padding_start: along(vertical, snapport.y - scrollport.y, snapport.x - scrollport.x),
            scroll_padding_end: along(
                vertical,
                bottom(scrollport) - bottom(snapport),
                right(scrollport) - right(snapport),
            ),
        })
    }

    fn followed_scroller(&self) -> Option<FollowedScroller> {
        Some(FollowedScroller {
            scroll_node: self.scroll_node?,
            scroller_style_node: self.scroller_style_node,
            vertical: self.is_vertical,
            scroll_offset: self.scroll_offset,
            max_scroll_offset: self.max_scroll_offset,
        })
    }
}

/// Where the subject of a view timeline is, along the timeline's axis, as the host laid it out: where its border box
/// starts, in the coordinates the boxes inside the scrollport are laid out in, and its size, in CSS pixels.
#[derive(Clone, Copy, Debug)]
pub(crate) struct SubjectMetrics {
    pub(crate) start: f64,
    pub(crate) size: f64,
}

impl SubjectMetrics {
    /// The subject whose box `arena` lays out in `subject`, along the vertical axis where `vertical`, and along the
    /// horizontal one otherwise, where it has a box.
    pub(crate) fn measure(arena: &LayoutNodeArena, subject: NodeSlotId, vertical: bool) -> Option<Self> {
        if !arena.paintable_row_is_populated(subject) {
            return None;
        }
        // NB: The border box is laid out where the scroll container's scrolled content is at its scroll origin, with
        //     transforms ignored and relative positioning accounted for, as the view progress timeline ranges ask.
        let border_box = first_fragment_border_box_rect(arena, subject);
        Some(Self {
            start: along(vertical, border_box.y, border_box.x),
            size: along(vertical, border_box.height, border_box.width),
        })
    }
}

/// Whether the scroll axis `axis`, the host's `ScrollAxis`, of a scroll container whose computed `writing-mode` and
/// `direction` are `writing_mode` and `direction` is vertical, and whether it is reversed.
fn computed_scroll_axis(axis: u8, writing_mode: u8, direction: u8) -> (bool, bool) {
    // NB: This is based on the table specified here: https://drafts.csswg.org/css-writing-modes-4/#logical-to-physical

    // FIXME: Note: The used direction depends on the computed writing-mode and text-orientation: in vertical writing
    //              modes, a text-orientation value of upright forces the used direction to ltr.
    let used_direction = direction;

    match axis {
        SCROLL_AXIS_BLOCK => match writing_mode {
            writing_mode::HORIZONTAL_TB => (true, false),
            writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL => (false, true),
            writing_mode::VERTICAL_LR | writing_mode::SIDEWAYS_LR => (false, false),
            _ => unreachable!("a writing mode the style engine computes"),
        },
        SCROLL_AXIS_INLINE => match writing_mode {
            writing_mode::HORIZONTAL_TB => (false, used_direction == direction::RTL),
            writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL | writing_mode::VERTICAL_LR => {
                (true, used_direction == direction::RTL)
            }
            writing_mode::SIDEWAYS_LR => (true, used_direction == direction::LTR),
            _ => unreachable!("a writing mode the style engine computes"),
        },
        SCROLL_AXIS_X => (false, false),
        SCROLL_AXIS_Y => (true, false),
        _ => unreachable!("the host names a scroll axis it knows"),
    }
}

/// The host's `ScrollAxis`.
/// https://drafts.csswg.org/scroll-animations-1/#enumdef-scrollaxis
const SCROLL_AXIS_BLOCK: u8 = 0;
const SCROLL_AXIS_INLINE: u8 = 1;
const SCROLL_AXIS_X: u8 = 2;
const SCROLL_AXIS_Y: u8 = 3;

/// The boxes a scroll timeline follows, as the host found them from its source, and from its subject for a view
/// timeline.
pub(crate) struct ScrollTimelineBoxes {
    /// The box of the scroll container, if any: the viewport's where the source is the root element.
    pub(crate) scroller: NodeSlotId,
    pub(crate) scroll_node: Option<ScrollNodeStableId>,
    /// The style node of the element whose box scrolls, or 0 for the viewport.
    pub(crate) scroller_style_node: u32,
    /// The box of a view timeline's subject, if any.
    pub(crate) subject: NodeSlotId,
}

/// What decides the progress of a view timeline, along its axis, in CSS pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ViewTimelineMetrics {
    pub(crate) followed_scroller: Option<FollowedScroller>,
    pub(crate) scroll_offset: f64,
    pub(crate) max_scroll_offset: f64,
    pub(crate) scrollport_size: f64,
    /// The insets of the view progress visibility range, at the start and at the end of the scrollport.
    pub(crate) inset_start: f64,
    pub(crate) inset_end: f64,
    /// Where the subject's border box starts, from the start of the scrollport, and its size.
    pub(crate) subject_offset: f64,
    pub(crate) subject_size: f64,
}

/// The scroll offsets at the start and at the end of a named timeline range, in CSS pixels.
struct ScrollOffsetInterval {
    start: f64,
    end: f64,
}

/// https://drafts.csswg.org/scroll-animations-1/#scroll-timeline-progress
/// The progress of a scroll progress timeline whose scroller is at the scroll offset `position`, of which
/// `max_scroll_offset` is at 100% progress, where those positions do not coincide.
pub(crate) fn scroll_progress_at(position: f64, max_scroll_offset: f64) -> Option<TimeValue> {
    // If the 0% position and 100% position coincide (i.e. the denominator in the current time formula is zero), the
    // timeline is inactive.
    if max_scroll_offset == 0.0 {
        return None;
    }

    // Progress (the current time) for a scroll progress timeline is calculated as:
    //     scroll offset ÷ (scrollable overflow size − scroll container size)
    let progress = position / max_scroll_offset;
    Some(TimeValue::percentage(progress * 100.0))
}

impl ViewTimelineMetrics {
    /// What decides the progress of a view timeline whose scroller is `scroller` and whose subject is `subject`, with
    /// the insets of its view progress visibility range resolved to `inset_start` and `inset_end`.
    fn new(scroller: &ScrollerMetrics, subject: &SubjectMetrics, inset_start: f64, inset_end: f64) -> Self {
        // https://drafts.csswg.org/scroll-animations-1/#valdef-animation-timeline-range-cover
        // 0% progress represents the latest position at which the start border edge of the element’s principal box
        // coincides with the end edge of its view progress visibility range.
        // 100% progress represents the earliest position at which the end border edge of the element’s principal box
        // coincides with the start edge of its view progress visibility range.
        Self {
            followed_scroller: scroller.followed_scroller(),
            scroll_offset: scroller.scroll_offset,
            max_scroll_offset: scroller.max_scroll_offset,
            scrollport_size: scroller.scrollport_size,
            inset_start,
            inset_end,
            subject_offset: subject.start - scroller.scrollport_start,
            subject_size: subject.size,
        }
    }

    /// The start of the cover range.
    pub(crate) fn cover_start(&self) -> f64 {
        self.subject_offset - (self.scrollport_size - self.inset_end)
    }

    /// The end of the cover range.
    pub(crate) fn cover_end(&self) -> f64 {
        self.subject_offset + self.subject_size - self.inset_start
    }

    /// https://drafts.csswg.org/scroll-animations-1/#view-timelines-ranges
    fn interval_for_range_name(&self, name: TimelineRangeName) -> ScrollOffsetInterval {
        let sorted = |start: f64, end: f64| {
            if end < start {
                return ScrollOffsetInterval { start: end, end: start };
            }
            ScrollOffsetInterval { start, end }
        };
        match name {
            TimelineRangeName::None | TimelineRangeName::Cover => {
                // Represents the full range of the view progress timeline.
                ScrollOffsetInterval {
                    start: self.cover_start(),
                    end: self.cover_end(),
                }
            }
            TimelineRangeName::Contain => {
                // Represents the range during which the principal box is either fully contained by, or fully covers,
                // its view progress visibility range within the scrollport.
                sorted(
                    self.cover_start() + self.subject_size,
                    self.cover_end() - self.subject_size,
                )
            }
            TimelineRangeName::Entry => {
                // 0% is equivalent to 0% of the cover range.
                // 100% is equivalent to 0% of the contain range.
                let contain = self.interval_for_range_name(TimelineRangeName::Contain);
                sorted(self.cover_start(), contain.start)
            }
            TimelineRangeName::Exit => {
                // 0% is equivalent to 100% of the contain range.
                // 100% is equivalent to 100% of the cover range.
                let contain = self.interval_for_range_name(TimelineRangeName::Contain);
                sorted(contain.end, self.cover_end())
            }
            TimelineRangeName::EntryCrossing => {
                // Represents the range during which the principal box crosses the end border edge.
                sorted(self.cover_start(), self.cover_start() + self.subject_size)
            }
            TimelineRangeName::ExitCrossing => {
                // Represents the range during which the principal box crosses the start border edge.
                sorted(self.cover_end() - self.subject_size, self.cover_end())
            }
            TimelineRangeName::Scroll => {
                // Represents the full range of the scroll container on which the view progress timeline is defined.
                ScrollOffsetInterval {
                    start: 0.0,
                    end: self.max_scroll_offset,
                }
            }
        }
    }

    /// The timeline time at the scroll offset `position`, as a proportion of the cover range, which spans the entire
    /// timeline, where the cover range is not empty.
    fn time_at(&self, position: f64) -> Option<TimeValue> {
        let cover_length = self.cover_end() - self.cover_start();
        if cover_length == 0.0 {
            return None;
        }
        Some(TimeValue::percentage(
            (position - self.cover_start()) / cover_length * 100.0,
        ))
    }
}

/// The source of a scroll timeline.
/// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ScrollTimelineSource {
    /// The host's `DOM::Element` script or a named timeline gave, if any.
    Element(Option<NonNull<c_void>>),
    /// The scroller, the host's `CSS::Scroller`, that an anonymous timeline finds from `target`, the host's
    /// `DOM::Element`, or its pseudo-element, the host's `PseudoElement`.
    /// https://drafts.csswg.org/scroll-animations-1/#scroll-notation
    Anonymous {
        scroller: u8,
        target: NonNull<c_void>,
        pseudo_element: Option<u8>,
    },
}

/// What a view timeline adds to the scroll timeline it is.
/// https://drafts.csswg.org/scroll-animations-1/#viewtimeline
pub(crate) struct ViewTimelineState {
    /// https://drafts.csswg.org/scroll-animations-1/#view-timeline-inset
    pub(crate) inset: ViewTimelineInset,
    /// What the timeline's progress was last calculated from: none while it was inactive.
    pub(crate) last_metrics: Option<ViewTimelineMetrics>,
    /// The style node by which a clock tick finds the subject's box, as of the timeline's last update, unless the
    /// subject is a pseudo-element.
    pub(crate) subject_style_node: Option<StyleNodeID>,
}

/// The state of a scroll timeline, and of a view timeline, which is one.
/// https://drafts.csswg.org/scroll-animations-1/#scrolltimeline
pub(crate) struct ScrollTimelineState {
    /// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-source
    pub(crate) source: ScrollTimelineSource,
    /// https://drafts.csswg.org/scroll-animations-1/#dom-scrolltimeline-axis
    /// The host's `ScrollAxis`.
    pub(crate) axis: u8,
    /// The maximum scroll offset at the last update of a scroll progress timeline, while it was active.
    pub(crate) last_max_scroll_offset: Option<f64>,
    /// The scroller the timeline followed at its last update, while it was active.
    pub(crate) followed_scroller: Option<FollowedScroller>,
    /// What a view timeline adds.
    pub(crate) view: Option<ViewTimelineState>,
}

impl ScrollTimelineState {
    /// What decides the progress of the view timeline this is, where it is not inactive, from where the host laid out
    /// its scroller and its subject.
    fn view_metrics(
        &self,
        scroller: Option<&ScrollerMetrics>,
        subject: Option<&SubjectMetrics>,
    ) -> Option<ViewTimelineMetrics> {
        let view = self.view.as_ref().expect("a view timeline has a subject");
        // If the source or subject of a ViewTimeline is an element whose principal box does not exist, or if its
        // nearest ancestor scroll container has no scrollable overflow or is not scrollable in the specified axis (or
        // if there is no such ancestor, e.g. in print media), then the ViewTimeline is inactive.
        let (scroller, subject) = (scroller?, subject?);
        Some(ViewTimelineMetrics::new(
            scroller,
            subject,
            view.inset
                .start
                .resolve(scroller.scrollport_size, scroller.scroll_padding_start),
            view.inset
                .end
                .resolve(scroller.scrollport_size, scroller.scroll_padding_end),
        ))
    }

    /// Where `arena` lays out the scroller of the timeline in `boxes`, if its box is a scroll container with scrollable
    /// overflow, and, for a view timeline, its subject, if it has a box.
    pub(crate) fn measure(
        &self,
        arena: &LayoutNodeArena,
        boxes: &ScrollTimelineBoxes,
    ) -> (Option<ScrollerMetrics>, Option<SubjectMetrics>) {
        let Some(style) = arena
            .node(boxes.scroller)
            .filter(|_| crate::painting::style_queries::is_scroll_container(arena, boxes.scroller))
            .and_then(|row| row.style())
        else {
            return (None, None);
        };
        let (vertical, _reversed) = computed_scroll_axis(self.axis, style.writing_mode(), style.direction());
        let Some(scroller) = ScrollerMetrics::measure(
            arena,
            boxes.scroller,
            boxes.scroll_node,
            boxes.scroller_style_node,
            vertical,
            self.view.is_some(),
        ) else {
            return (None, None);
        };
        let subject = self
            .view
            .as_ref()
            .and_then(|_| SubjectMetrics::measure(arena, boxes.subject, vertical));
        (Some(scroller), subject)
    }

    /// The timeline times of the start and the end of the attachment range `range`.
    /// https://drafts.csswg.org/scroll-animations-1/#animation-attachment-range
    fn interval_for_attachment_range(&self, range: &[AttachmentRangeEdge; 2]) -> AttachmentInterval {
        if let Some(metrics) = self.view.as_ref().and_then(|view| view.last_metrics)
            && metrics.time_at(0.0).is_some()
        {
            // NB: Each edge is measured from the start of its named timeline range, along that range, and is then
            //     placed as a proportion of the cover range, which spans the entire timeline.
            let time_of = |edge: &AttachmentRangeEdge| {
                let named_range = metrics.interval_for_range_name(TimelineRangeName::from_host(edge.name));
                let position = named_range.start + edge.offset.offset_along(named_range.end - named_range.start);
                metrics.time_at(position).expect("the cover range is not empty")
            };
            return AttachmentInterval {
                start: time_of(&range[0]),
                end: time_of(&range[1]),
            };
        }

        // NB: A scroll progress timeline has no named timeline ranges, so each edge is measured along the entire
        //     timeline, whose length is the maximum scroll offset. This matches WebKit.
        let max_scroll_offset = self.last_max_scroll_offset.unwrap_or(0.0);
        let time_of = |edge: &AttachmentRangeEdge| {
            if max_scroll_offset == 0.0 {
                return TimeValue::percentage(edge.offset.percentage);
            }
            TimeValue::percentage(edge.offset.offset_along(max_scroll_offset) / max_scroll_offset * 100.0)
        };
        AttachmentInterval {
            start: time_of(&range[0]),
            end: time_of(&range[1]),
        }
    }
}

/// A scroll timeline as a rendering update seals it into the render clock's plan, whose ticks sample its animations
/// where the compositor has scrolled its scroller to, and follow the layout the ticks' frames make of it.
#[derive(Clone, Copy, Debug)]
pub(crate) struct ScrollTimelineSnapshot {
    pub(crate) timeline: AnimationTimelineId,
    /// The scroller the timeline followed as the plan was sealed, with its scroll offset at 100% progress in the
    /// layout the ticks' frames last made of it.
    pub(crate) scroller: FollowedScroller,
    view: Option<ViewTimelineSnapshot>,
}

/// What a view timeline adds to the scroll timeline it is, as the render clock's plan seals it.
#[derive(Clone, Copy, Debug)]
struct ViewTimelineSnapshot {
    subject: StyleNodeID,
    inset_start: LinearInsetEdge,
    inset_end: LinearInsetEdge,
    /// What decides the timeline's progress, in the layout the ticks' frames last made of it.
    metrics: ViewTimelineMetrics,
}

impl ScrollTimelineSnapshot {
    /// The progress, in percent, of the timeline at the scroll offset `position`.
    /// NB: The 0% and 100% positions never coincide: the plan seals no such timeline, and `follow_layout` leaves one
    ///     where they would.
    pub(crate) fn progress_at(&self, position: f64) -> f64 {
        let progress = match &self.view {
            None => scroll_progress_at(position, self.scroller.max_scroll_offset),
            Some(view) => view.metrics.time_at(position),
        };
        progress.expect("the 0% and 100% positions do not coincide").value
    }

    /// Has the timeline follow the layout `arena` holds, as a host's layout decides its next rendering update's
    /// samples. What makes the timeline inactive there leaves it where it was: the host decides that.
    pub(crate) fn follow_layout(&mut self, arena: &LayoutNodeArena) {
        let scroller = StyleNodeID::from_raw(self.scroller.scroller_style_node)
            .map_or_else(|| arena.layout_root(), |element| arena.bound_row(element));
        let Some(scroller) = ScrollerMetrics::measure(
            arena,
            scroller,
            Some(self.scroller.scroll_node),
            self.scroller.scroller_style_node,
            self.scroller.vertical,
            self.view.is_some(),
        ) else {
            return;
        };
        if scroll_progress_at(0.0, scroller.max_scroll_offset).is_none() {
            return;
        }
        self.scroller.max_scroll_offset = scroller.max_scroll_offset;
        let Some(view) = &mut self.view else {
            return;
        };
        let Some(subject) = SubjectMetrics::measure(arena, arena.bound_row(view.subject), self.scroller.vertical)
        else {
            return;
        };
        let metrics = ViewTimelineMetrics::new(
            &scroller,
            &subject,
            view.inset_start
                .resolve(scroller.scrollport_size, scroller.scroll_padding_start),
            view.inset_end
                .resolve(scroller.scrollport_size, scroller.scroll_padding_end),
        );
        if metrics.time_at(0.0).is_some() {
            view.metrics = metrics;
        }
    }
}

/// What kind of timeline a timeline is, with the state of its kind.
pub(crate) enum TimelineKind {
    /// https://www.w3.org/TR/web-animations-1/#document-timelines
    Document {
        /// https://www.w3.org/TR/web-animations-1/#origin-time
        origin_time: f64,
    },
    /// A scroll progress timeline, or a view progress timeline where it has view state.
    Scroll(Box<ScrollTimelineState>),
    /// The timeline of the Internals interface, whose time tests set.
    Internal {
        /// The time a read of the timeline observes, where one is set.
        time_for_observation: Option<TimeValue>,
    },
}

/// What script observed of a timeline in a task.
#[derive(Clone, Copy, Default)]
struct TimelineObservation {
    /// The task generation of the event loop in which the timeline was last updated or observed.
    task_generation: Option<u64>,
    current_time: Option<TimeValue>,
}

/// The state of a timeline.
/// https://www.w3.org/TR/web-animations-1/#the-animationtimeline-interface
pub(crate) struct TimelineRecord {
    id: AnimationTimelineId,
    /// https://www.w3.org/TR/web-animations-1/#timeline-associated-with-a-document
    /// The host's `DOM::Document`, which the host's timeline keeps alive.
    document: NonNull<c_void>,
    /// https://www.w3.org/TR/web-animations-1/#dom-animationtimeline-currenttime
    current_time: Option<TimeValue>,
    /// https://drafts.csswg.org/web-animations-1/#monotonically-increasing-timeline
    is_monotonically_increasing: bool,
    observation: TimelineObservation,
    /// The current time a style sampling reads instead of the current time, where it is set.
    style_sampling_override: Option<Option<TimeValue>>,
    pub(crate) kind: TimelineKind,
}

impl TimelineRecord {
    #[must_use]
    pub(crate) fn scroll(&self) -> Option<&ScrollTimelineState> {
        match &self.kind {
            TimelineKind::Scroll(scroll) => Some(scroll),
            TimelineKind::Document { .. } | TimelineKind::Internal { .. } => None,
        }
    }

    fn scroll_mut(&mut self) -> &mut ScrollTimelineState {
        match &mut self.kind {
            TimelineKind::Scroll(scroll) => scroll,
            TimelineKind::Document { .. } | TimelineKind::Internal { .. } => {
                unreachable!("the timeline is a scroll timeline")
            }
        }
    }

    /// https://drafts.csswg.org/web-animations-1/#monotonically-increasing-timeline
    #[must_use]
    pub(crate) fn is_monotonically_increasing(&self) -> bool {
        self.is_monotonically_increasing
    }

    /// The current time a style sampling reads instead of the current time, where it is set.
    #[must_use]
    pub(crate) fn style_sampling_override(&self) -> Option<Option<TimeValue>> {
        self.style_sampling_override
    }

    pub(crate) fn set_style_sampling_override(&mut self, current_time: Option<Option<TimeValue>>) {
        self.style_sampling_override = current_time;
    }

    fn effective_current_time(&self) -> Option<TimeValue> {
        match self.style_sampling_override {
            Some(current_time) => current_time,
            None => self.current_time,
        }
    }

    /// https://drafts.csswg.org/web-animations-2/#progress-based-timeline
    #[must_use]
    pub(crate) fn is_progress_based(&self) -> bool {
        matches!(self.kind, TimelineKind::Scroll(_))
    }

    /// https://drafts.csswg.org/web-animations-2/#timeline-duration
    #[must_use]
    pub(crate) fn duration(&self) -> Option<TimeValue> {
        // The duration of a timeline gives the maximum value a timeline may generate for its current time. This value
        // is used to calculate the intrinsic iteration duration for the target effect of an animation that is
        // associated with the timeline when the effect’s iteration duration is "auto". The value is computed such that
        // the effect fills the available time. For a monotonic timeline, there is no upper bound on current time, and
        // timeline duration is unresolved. For a non-monotonic (e.g. scroll) timeline, the duration has a fixed upper
        // bound. In this case, the timeline is a progress-based timeline, and its timeline duration is 100%.
        self.is_progress_based().then_some(TimeValue::percentage(100.0))
    }

    /// https://drafts.csswg.org/web-animations-2/#minimum-timeline-time
    /// NB: None for a monotonically increasing timeline, whose minimum timeline time is -infinity.
    fn minimum_timeline_time(&self) -> Option<TimeValue> {
        let scroll = self.scroll()?;
        match &scroll.view {
            // NB: The timeline times at the minimum and the maximum scroll offsets, which need not be 0% and 100%.
            Some(view) => view.last_metrics.and_then(|metrics| metrics.time_at(0.0)),
            // NB: The minimum and the maximum scroll offsets.
            None => Some(TimeValue::percentage(0.0)),
        }
    }

    /// https://drafts.csswg.org/web-animations-2/#maximum-timeline-time
    /// NB: None for a monotonically increasing timeline, whose maximum timeline time is +infinity.
    fn maximum_timeline_time(&self) -> Option<TimeValue> {
        let scroll = self.scroll()?;
        match &scroll.view {
            Some(view) => view
                .last_metrics
                .and_then(|metrics| metrics.time_at(metrics.max_scroll_offset)),
            None => Some(TimeValue::percentage(100.0)),
        }
    }

    /// The current time at `timestamp` of a timeline that can sample it there, and its current time otherwise.
    fn current_time_at_timestamp(&self, timestamp: f64, is_inactive: bool) -> Option<TimeValue> {
        match &self.kind {
            TimelineKind::Document { origin_time } => {
                (!is_inactive).then(|| TimeValue::milliseconds(timestamp - origin_time))
            }
            TimelineKind::Internal { time_for_observation } => *time_for_observation,
            TimelineKind::Scroll(_) => (!is_inactive).then_some(self.effective_current_time()).flatten(),
        }
    }

    /// Whether a read of the timeline can sample its current time at a timestamp of its own.
    #[must_use]
    pub(crate) fn can_sample_current_time_at_timestamp(&self) -> bool {
        match &self.kind {
            TimelineKind::Document { .. } => true,
            TimelineKind::Internal { time_for_observation } => time_for_observation.is_some(),
            TimelineKind::Scroll(_) => false,
        }
    }

    /// The task generation in which script observed the timeline at a time ahead of its current time, if any.
    #[must_use]
    pub(crate) fn observed_ahead_in_task_generation(&self) -> Option<u64> {
        if self.observation.current_time == self.current_time {
            return None;
        }
        self.observation.task_generation
    }

    /// The task generation of the event loop in which the timeline was last updated or observed, if any, and its
    /// current time then.
    #[must_use]
    pub(crate) fn observed_in_task_generation(&self) -> (Option<u64>, Option<TimeValue>) {
        (self.observation.task_generation, self.observation.current_time)
    }

    /// Sets the current time of the timeline in the task of `task_generation`, answering whether it took it: a
    /// monotonically increasing timeline only moves forward.
    fn set_current_time(&mut self, value: Option<TimeValue>, task_generation: u64) -> bool {
        if self.is_monotonically_increasing
            && let Some(current_time) = self.current_time
            && value.is_none_or(|value| value < current_time)
        {
            return false;
        }
        self.current_time = value;
        self.observation = TimelineObservation {
            task_generation: Some(task_generation),
            current_time: value,
        };
        true
    }
}

/// A timeline's record, which every handle to the timeline shares, the animations associated with it, and the host's
/// timeline of it.
pub struct TimelineCell {
    record: RefCell<TimelineRecord>,
    /// NB: The animations associated with the timeline, none of which it keeps alive.
    associated_animations: RefCell<WeakAnimationSet>,
    /// OPTIMIZATION: The animations associated with the timeline that an update of its current time may change, none of
    ///               which it keeps alive. An animation that has finished on a monotonically increasing timeline, with
    ///               no pending task, stays as it is until a procedure runs on it, which adds it back.
    animations_needing_update: RefCell<WeakAnimationSet>,
    /// The cell of the object script sees of the timeline, which holds a reference to the record until it is finalized
    /// and clears this as it does, if script has asked for it yet. Each holder of the record keeps it alive, or the
    /// cells the record names where it has none.
    object_cell: Cell<Option<NonNull<c_void>>>,
}

/// How a timeline came to have its current time.
pub(crate) enum TimelineUpdate<'a> {
    /// A document timeline's at the now timestamp of the update animations and send events procedure.
    Timestamp(f64),
    /// A scroll timeline's from where the host laid out its scroller, if it has a box that is a scroll container with
    /// scrollable overflow, and, for a view timeline, its subject, if it has a box, which a clock tick finds by
    /// `subject_style_node`.
    Layout {
        scroller: Option<&'a ScrollerMetrics>,
        subject: Option<&'a SubjectMetrics>,
        subject_style_node: Option<StyleNodeID>,
    },
}

impl TimelineCell {
    pub(crate) fn borrow(&self) -> Ref<'_, TimelineRecord> {
        self.record.borrow()
    }

    pub(crate) fn borrow_mut(&self) -> RefMut<'_, TimelineRecord> {
        self.record.borrow_mut()
    }

    /// The cell of the object script sees of the timeline, while it has one.
    pub(crate) fn object_cell(&self) -> Option<NonNull<c_void>> {
        self.object_cell.get()
    }

    /// Calls `visit` with each GC cell of the host the record names: the document the timeline is associated with, and
    /// the element a scroll timeline finds its scroller from, if any, which the object script sees of the timeline
    /// visits.
    pub(crate) fn for_each_host_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        let record = self.borrow();
        visit(record.document, HostCellKind::Cell);
        match record.scroll().map(|scroll| scroll.source) {
            Some(ScrollTimelineSource::Element(Some(element))) => visit(element, HostCellKind::Element),
            Some(ScrollTimelineSource::Anonymous { target, .. }) => visit(target, HostCellKind::Element),
            Some(ScrollTimelineSource::Element(None)) | None => {}
        }
    }

    /// Calls `visit` with each GC cell of the host that keeps alive what the record names: the object script sees of
    /// the timeline, or, for a timeline that has none, the cells the record names. A holder of the record the GC traces
    /// visits these.
    pub(crate) fn for_each_holder_cell(&self, mut visit: impl FnMut(NonNull<c_void>, HostCellKind)) {
        if let Some(object_cell) = self.object_cell() {
            visit(object_cell, HostCellKind::Cell);
            return;
        }
        self.for_each_host_cell(visit);
    }

    /// The host's `DOM::Document` the timeline is associated with.
    pub(crate) fn document(&self) -> NonNull<c_void> {
        self.borrow().document
    }

    pub(crate) fn associated_animations(&self) -> &RefCell<WeakAnimationSet> {
        &self.associated_animations
    }

    pub(crate) fn animations_needing_update(&self) -> &RefCell<WeakAnimationSet> {
        &self.animations_needing_update
    }

    /// The animations associated with the timeline that an update of its current time may change, in the order of the
    /// animations associated with it, forgetting that the others need updates.
    pub(crate) fn associated_animations_needing_update(&self) -> Vec<AnimationHandle> {
        let associated = self.associated_animations.borrow();
        let mut needing_update = self.animations_needing_update.borrow_mut();
        let mut animations = Vec::new();
        for animation in needing_update.alive() {
            match associated.position(&animation) {
                Some(position) => animations.push((position, animation)),
                None => {
                    needing_update.remove(&animation);
                }
            }
        }
        animations.sort_unstable_by_key(|(position, _)| *position);
        animations.into_iter().map(|(_, animation)| animation).collect()
    }

    /// https://drafts.csswg.org/web-animations-1/#timeline
    #[must_use]
    pub(crate) fn is_inactive(&self) -> bool {
        let record = self.borrow();
        // A timeline is considered to be inactive when its time value is unresolved, and active otherwise.
        if record.effective_current_time().is_none() {
            return true;
        }
        if let TimelineKind::Document { .. } = record.kind {
            // https://www.w3.org/TR/web-animations-1/#document-timelines
            // A document timeline that is associated with a Document that is not an active document is also considered
            // to be inactive.
            return !super::ffi::document_is_active(record.document);
        }
        false
    }

    /// https://drafts.csswg.org/web-animations-1/#dom-animationtimeline-currenttime
    #[must_use]
    pub(crate) fn current_time(&self) -> Option<TimeValue> {
        // Returns the current time for this timeline or null if this timeline is inactive.
        if self.is_inactive() {
            return None;
        }
        self.borrow().effective_current_time()
    }

    /// What the timing model reads of the timeline now.
    #[must_use]
    pub(crate) fn sample(&self) -> TimelineSample {
        let current_time = self.current_time();
        let record = self.borrow();
        TimelineSample {
            current_time: current_time.into(),
            minimum_timeline_time: record.minimum_timeline_time().into(),
            maximum_timeline_time: record.maximum_timeline_time().into(),
            is_monotonically_increasing: record.is_monotonically_increasing,
        }
    }

    /// What moves the timeline as a clock tick samples it.
    #[must_use]
    pub(crate) fn clock(&self) -> TimelineClock {
        // NB: A document timeline's time is a timestamp less its origin time, which lets a clock tick sample it.
        if let Some(origin_time) =
            self.convert_a_timeline_time_to_an_origin_relative_time(Some(TimeValue::milliseconds(0.0)))
        {
            return TimelineClock {
                source: TimelineClockSource::Document,
                origin_time,
                scroll_timeline: AnimationTimelineId::default(),
            };
        }
        let record = self.borrow();
        match &record.kind {
            // NB: A scroll timeline's time is the progress of the scroller it follows, which is what lets a clock tick
            //     sample it where the compositor has scrolled to.
            TimelineKind::Scroll(scroll) if scroll.followed_scroller.is_some() => TimelineClock {
                source: TimelineClockSource::Scroll,
                origin_time: 0.0,
                scroll_timeline: record.id,
            },
            TimelineKind::Document { .. } | TimelineKind::Scroll(_) | TimelineKind::Internal { .. } => {
                TimelineClock::default()
            }
        }
    }

    /// The timeline as the render clock's plan seals it, where a clock tick can follow it: a scroll timeline that
    /// follows a scroller, and of a view timeline, one whose subject a tick finds and whose insets it resolves.
    #[must_use]
    pub(crate) fn clock_snapshot(&self) -> Option<ScrollTimelineSnapshot> {
        let current_time = self.current_time()?;
        if current_time.r#type != super::timing::TimeValueType::Percentage {
            return None;
        }
        let record = self.borrow();
        let scroll = record.scroll()?;
        let scroller = scroll.followed_scroller?;
        let view = match &scroll.view {
            None => None,
            Some(view) => Some(ViewTimelineSnapshot {
                // NB: A tick finds the subject of a view timeline by its box, which a pseudo-element does not name.
                subject: view.subject_style_node?,
                // NB: A tick resolves the insets of a view timeline as a length plus a percentage, which not every
                //     calculation is.
                inset_start: view.inset.start.as_linear()?,
                inset_end: view.inset.end.as_linear()?,
                metrics: view.last_metrics?,
            }),
        };
        Some(ScrollTimelineSnapshot {
            timeline: record.id,
            scroller,
            view,
        })
    }

    /// The timeline as an animation attached to `range` of it reads it now.
    #[must_use]
    pub(crate) fn state_for(&self, range: &[AttachmentRangeEdge; 2]) -> TimelineState {
        let sample = self.sample();
        let record = self.borrow();
        TimelineState {
            sample,
            duration: record.duration(),
            attachment_interval: record
                .scroll()
                .map(|scroll| scroll.interval_for_attachment_range(range)),
            is_progress_based: record.is_progress_based(),
        }
    }

    /// https://www.w3.org/TR/web-animations-1/#timeline-time-to-origin-relative-time
    #[must_use]
    pub(crate) fn convert_a_timeline_time_to_an_origin_relative_time(
        &self,
        timeline_time: Option<TimeValue>,
    ) -> Option<f64> {
        let TimelineKind::Document { origin_time } = self.borrow().kind else {
            return None;
        };
        self.convert_a_timeline_time_to_an_origin_relative_time_from(timeline_time, origin_time)
    }

    /// https://www.w3.org/TR/web-animations-1/#ref-for-timeline-time-to-origin-relative-time
    fn convert_a_timeline_time_to_an_origin_relative_time_from(
        &self,
        timeline_time: Option<TimeValue>,
        origin_time: f64,
    ) -> Option<f64> {
        // To convert a timeline time, timeline time, to an origin-relative time for a document timeline, timeline,
        // return the sum of the timeline time and timeline’s origin time. If timeline is inactive, return an
        // unresolved time value.
        if self.is_inactive() {
            return None;
        }
        let timeline_time = timeline_time?;
        // NB: We know the timeline time of a DocumentTimeline is always in milliseconds because it's only ever set
        //     from update_current_time()
        assert!(timeline_time.r#type == super::timing::TimeValueType::Milliseconds);
        Some(timeline_time.value + origin_time)
    }

    /// The current time at `timestamp` of a timeline that can sample it there, and its current time otherwise. Where
    /// script observes it in the task of `observed_in_task_generation`, the timeline keeps it as what script observed
    /// in that task.
    pub(crate) fn current_time_at_timestamp(
        &self,
        timestamp: f64,
        observed_in_task_generation: Option<u64>,
    ) -> Option<TimeValue> {
        let is_inactive = self.is_inactive();
        let mut record = self.borrow_mut();
        let current_time = record.current_time_at_timestamp(timestamp, is_inactive);
        if let Some(task_generation) = observed_in_task_generation {
            record.observation = TimelineObservation {
                task_generation: Some(task_generation),
                current_time,
            };
        }
        current_time
    }

    /// Updates the current time of the timeline in the task of `task_generation`, as `update` has it, unless the
    /// update leaves it as it was where `only_if_stale`, answering whether it updated it. `update_associated_animations`
    /// updates the animations associated with the timeline as its current time changes.
    /// https://drafts.csswg.org/web-animations-1/#animation-frame-loop
    pub(crate) fn update_current_time(
        &self,
        update: TimelineUpdate<'_>,
        task_generation: u64,
        only_if_stale: bool,
        update_associated_animations: impl FnOnce(),
    ) -> bool {
        let mut record = self.borrow_mut();
        // NB: The Internals interface's timeline keeps the time its tests set, and only updates its animations.
        let current_time = match update {
            TimelineUpdate::Timestamp(timestamp) => match record.kind {
                TimelineKind::Document { origin_time } => {
                    // https://drafts.csswg.org/web-animations-1/#document-timeline
                    // A document timeline is a type of timeline that is associated with a document and whose current
                    // time is calculated as a fixed offset from the now timestamp provided each time the update
                    // animations and send events procedure is run. This fixed offset is equal to the current time of
                    // the default document timeline when this timeline’s current time was zero, and is thus referred
                    // to as the document timeline’s origin time.
                    Some(Some(TimeValue::milliseconds(timestamp - origin_time)))
                }
                TimelineKind::Internal { .. } => None,
                TimelineKind::Scroll(_) => unreachable!("a scroll timeline updates from the layout of its scroller"),
            },
            TimelineUpdate::Layout {
                scroller,
                subject,
                subject_style_node,
            } => {
                let scroll = record.scroll_mut();
                if let Some(view) = &mut scroll.view {
                    view.subject_style_node = subject_style_node;
                }
                if only_if_stale && !scroll.is_stale(scroller, subject) {
                    return false;
                }
                Some(scroll.take_layout(scroller, subject))
            }
        };
        let took_current_time =
            current_time.is_none_or(|current_time| record.set_current_time(current_time, task_generation));
        drop(record);
        if took_current_time {
            update_associated_animations();
        }

        // https://drafts.csswg.org/web-animations-1/#ref-for-active-timeline
        // After a document timeline becomes active, it is monotonically increasing.
        if matches!(self.borrow().kind, TimelineKind::Document { .. })
            && !self.borrow().is_monotonically_increasing
            && !self.is_inactive()
        {
            self.borrow_mut().is_monotonically_increasing = true;
        }
        true
    }

    /// Sets the current time of the Internals interface's timeline to `time`, in milliseconds, in the task of
    /// `task_generation`, or where `for_observation`, the time a read of the timeline observes.
    pub(crate) fn set_internal_time(
        &self,
        time: Option<f64>,
        for_observation: bool,
        task_generation: u64,
        update_associated_animations: impl FnOnce(),
    ) {
        let time = time.map(TimeValue::milliseconds);
        let mut record = self.borrow_mut();
        if for_observation {
            let TimelineKind::Internal { time_for_observation } = &mut record.kind else {
                unreachable!("the timeline is the Internals interface's");
            };
            *time_for_observation = time;
            record.observation.task_generation = None;
            return;
        }
        let took_current_time = record.set_current_time(time, task_generation);
        drop(record);
        if took_current_time {
            update_associated_animations();
        }
    }

    /// Sets the axis of a scroll timeline, and the inset of a view timeline, answering whether that changed either.
    pub(crate) fn set_axis_and_inset(&self, axis: u8, inset: Option<ViewTimelineInset>) -> bool {
        let mut record = self.borrow_mut();
        let scroll = record.scroll_mut();
        let inset_changed = match (&mut scroll.view, inset) {
            (Some(view), Some(inset)) if !view.inset.equals(&inset) => {
                view.inset = inset;
                true
            }
            _ => false,
        };
        let axis_changed = scroll.axis != axis;
        scroll.axis = axis;
        axis_changed || inset_changed
    }
}

impl ScrollTimelineState {
    /// https://drafts.csswg.org/scroll-animations-1/#stale-timelines
    /// Whether the layout of the scroller, and of the subject of a view timeline, changed the timeline since its last
    /// update.
    fn is_stale(&self, scroller: Option<&ScrollerMetrics>, subject: Option<&SubjectMetrics>) -> bool {
        // FIXME: This should probably be a spec bug

        // AD-HOC: The spec only lists two criteria for a scroll timeline to be considered stale: a) it was newly
        //         created within the current frame or; b) style update and layout within the current frame changed
        //         its associated named timeline ranges. However, WPT expects us to also consider the source element
        //         resizing as a cause for staleness, so we use a more broad definition of staleness and consider any
        //         timeline whose max scroll offset has changed since the last call to `update_current_time` to be
        //         stale. This matches the (admittedly narrow) tests in WPT and is the same behavior as is implemented
        //         by WebKit.
        if let Some(view) = &self.view {
            // NB: A view progress timeline's named timeline ranges move with its subject too.
            return self.view_metrics(scroller, subject) != view.last_metrics;
        }

        // FIXME: Account for the named timeline ranges of view progress timelines once they are implemented.
        scroller.map(|scroller| scroller.max_scroll_offset) != self.last_max_scroll_offset
    }

    /// Takes in where the host laid out the scroller, and the subject of a view timeline, answering the current time
    /// that makes the timeline.
    fn take_layout(
        &mut self,
        scroller: Option<&ScrollerMetrics>,
        subject: Option<&SubjectMetrics>,
    ) -> Option<TimeValue> {
        self.followed_scroller = None;
        if self.view.is_some() {
            return self.take_view_layout(scroller, subject);
        }

        // https://drafts.csswg.org/scroll-animations-1/#ref-for-dom-animationtimeline-currenttime
        // currentTime represents the scroll progress of the scroll container as a percentage CSSUnitValue, with 0%
        // representing its startmost scroll position (in the writing mode of the scroll container). Null when the
        // timeline is inactive.
        self.last_max_scroll_offset = scroller.map(|scroller| scroller.max_scroll_offset);

        // If the source of a ScrollTimeline is an element whose principal box does not exist or is not a scroll
        // container in the timeline's axis, or if there is no scrollable overflow, then the ScrollTimeline is inactive.
        let scroller = scroller?;

        // https://drafts.csswg.org/scroll-animations-1/#scroll-timeline-progress
        let progress = scroll_progress_at(scroller.scroll_offset, scroller.max_scroll_offset)?;

        // FIXME: In paged media, scroll progress timelines that would otherwise reference the document viewport are
        //        also inactive.

        self.followed_scroller = scroller.followed_scroller();
        Some(progress)
    }

    fn take_view_layout(
        &mut self,
        scroller: Option<&ScrollerMetrics>,
        subject: Option<&SubjectMetrics>,
    ) -> Option<TimeValue> {
        let metrics = self.view_metrics(scroller, subject);
        self.view.as_mut().expect("a view timeline has a subject").last_metrics = metrics;
        let metrics = metrics?;

        // https://drafts.csswg.org/scroll-animations-1/#view-timeline-progress
        // Progress (the current time) in a view progress timeline is calculated as: distance ÷ range where:
        // - distance is the current scroll offset minus the scroll offset corresponding to the start of the cover
        //   range
        // - range is the scroll offset corresponding to the end of the cover range minus the scroll offset
        //   corresponding to the start of the cover range
        // If the 0% position and 100% position coincide (i.e. the denominator in the current time formula is zero),
        // the timeline is inactive.
        // NB: The time at the scroll offset is that proportion of the cover range.
        let progress = metrics.time_at(metrics.scroll_offset)?;

        // FIXME: In paged media, view progress timelines that would otherwise reference the document viewport are
        //        also inactive.

        self.followed_scroller = metrics.followed_scroller;
        Some(progress)
    }
}

/// A strong reference to a timeline.
#[derive(Clone)]
pub(crate) struct TimelineHandle(Rc<TimelineCell>);

impl TimelineHandle {
    /// A new timeline of `kind`, associated with the host's `DOM::Document` `document`, whose host timeline is
    /// `object_cell`. Its current time is unresolved, and a document timeline is not monotonically increasing until it
    /// becomes active.
    pub(crate) fn new(document: NonNull<c_void>, kind: TimelineKind) -> Self {
        // NB: The Internals interface's timeline starts at zero and only moves forward.
        let (current_time, is_monotonically_increasing) = match kind {
            TimelineKind::Internal { .. } => (Some(TimeValue::milliseconds(0.0)), true),
            TimelineKind::Document { .. } | TimelineKind::Scroll(_) => (None, false),
        };
        Self(Rc::new(TimelineCell {
            record: RefCell::new(TimelineRecord {
                id: AnimationTimelineId::mint(),
                document,
                current_time,
                is_monotonically_increasing,
                observation: TimelineObservation::default(),
                style_sampling_override: None,
                kind,
            }),
            associated_animations: RefCell::default(),
            animations_needing_update: RefCell::default(),
            object_cell: Cell::new(None),
        }))
    }

    /// A new reference to the timeline the host holds a reference to.
    ///
    /// # Safety
    /// `cell` must be a timeline the host holds a reference to.
    pub(crate) unsafe fn retained_from_host(cell: &TimelineCell) -> Self {
        let cell = std::ptr::from_ref(cell);
        // SAFETY: Guaranteed by the caller.
        unsafe {
            Rc::increment_strong_count(cell);
            Self(Rc::from_raw(cell))
        }
    }

    /// Whether `self` and `other` are the same timeline.
    #[must_use]
    pub(crate) fn ptr_eq(&self, other: &Self) -> bool {
        Rc::ptr_eq(&self.0, &other.0)
    }

    /// Notes the cell of the object script sees of the timeline, or that it was finalized.
    pub(crate) fn set_object_cell(&self, object_cell: Option<NonNull<c_void>>) {
        self.0.object_cell.set(object_cell);
    }
}

impl std::ops::Deref for TimelineHandle {
    type Target = TimelineCell;

    fn deref(&self) -> &TimelineCell {
        &self.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn object_cell() -> NonNull<c_void> {
        NonNull::<u8>::dangling().cast()
    }

    fn edge(name: u8, px: f64, percentage: f64) -> AttachmentRangeEdge {
        AttachmentRangeEdge {
            name,
            offset: AttachmentRangeEdgeOffset {
                px,
                percentage,
                calculated: RetainedStyleValueData::none(),
            },
        }
    }

    fn auto_inset() -> ViewTimelineInset {
        let edge = ViewTimelineInsetEdge {
            is_auto: true,
            px: 0.0,
            percentage: 0.0,
            calculated: RetainedStyleValueData::none(),
        };
        ViewTimelineInset {
            start: edge.clone(),
            end: edge,
        }
    }

    fn scroll_timeline(view: Option<ViewTimelineInset>) -> TimelineHandle {
        TimelineHandle::new(
            object_cell(),
            TimelineKind::Scroll(Box::new(ScrollTimelineState {
                source: ScrollTimelineSource::Element(None),
                axis: 0,
                last_max_scroll_offset: None,
                followed_scroller: None,
                view: view.map(|inset| ViewTimelineState {
                    inset,
                    last_metrics: None,
                    subject_style_node: None,
                }),
            })),
        )
    }

    fn scroller(scroll_offset: f64, max_scroll_offset: f64) -> ScrollerMetrics {
        ScrollerMetrics {
            scroll_node: Some(ScrollNodeStableId {
                node_id: 1,
                kind: 1,
                pseudo_element_type: 0,
            }),
            scroller_style_node: 2,
            is_vertical: true,
            scroll_offset,
            max_scroll_offset,
            scrollport_start: 0.0,
            scrollport_size: 100.0,
            scroll_padding_start: 0.0,
            scroll_padding_end: 0.0,
        }
    }

    fn update(timeline: &TimelineHandle, scroller: Option<&ScrollerMetrics>, subject: Option<&SubjectMetrics>) {
        assert!(timeline.update_current_time(
            TimelineUpdate::Layout {
                scroller,
                subject,
                subject_style_node: None,
            },
            1,
            false,
            || {}
        ));
    }

    #[test]
    fn scroll_progress_is_the_scroll_offset_over_the_maximum() {
        let timeline = scroll_timeline(None);
        update(&timeline, Some(&scroller(50.0, 200.0)), None);
        assert_eq!(timeline.current_time(), Some(TimeValue::percentage(25.0)));
        assert_eq!(timeline.clock().source, TimelineClockSource::Scroll);

        // Without scrollable overflow the timeline is inactive, and nothing moves it.
        update(&timeline, Some(&scroller(0.0, 0.0)), None);
        assert_eq!(timeline.current_time(), None);
        assert_eq!(timeline.clock().source, TimelineClockSource::Unticked);
    }

    #[test]
    fn an_attachment_range_on_a_scroll_timeline_spans_the_scroll_range() {
        let timeline = scroll_timeline(None);
        update(&timeline, Some(&scroller(0.0, 200.0)), None);
        let state = timeline.state_for(&[edge(0, 20.0, 0.0), edge(0, 0.0, 50.0)]);
        let interval = state.attachment_interval().unwrap();
        assert_eq!(interval.start, TimeValue::percentage(10.0));
        assert_eq!(interval.end, TimeValue::percentage(50.0));
        assert_eq!(state.duration, Some(TimeValue::percentage(100.0)));
    }

    #[test]
    fn view_progress_runs_through_the_cover_range() {
        let mut inset = auto_inset();
        inset.start = ViewTimelineInsetEdge {
            is_auto: false,
            px: 10.0,
            percentage: 0.0,
            calculated: RetainedStyleValueData::none(),
        };
        let timeline = scroll_timeline(Some(inset));
        // A scrollport of 100px with 20px of scroll padding at its end, and a subject of 50px at 300px.
        let mut metrics = scroller(250.0, 1000.0);
        metrics.scroll_padding_end = 20.0;
        let subject = SubjectMetrics {
            start: 300.0,
            size: 50.0,
        };
        update(&timeline, Some(&metrics), Some(&subject));
        // The cover range runs from 300 - (100 - 20) = 220 to 300 + 50 - 10 = 340.
        let progress = (250.0 - 220.0) / 120.0 * 100.0;
        assert_eq!(timeline.current_time(), Some(TimeValue::percentage(progress)));
        let sample = timeline.sample();
        assert_eq!(
            sample.minimum_timeline_time.get(),
            Some(TimeValue::percentage((0.0 - 220.0) / 120.0 * 100.0))
        );
        assert_eq!(
            sample.maximum_timeline_time.get(),
            Some(TimeValue::percentage((1000.0 - 220.0) / 120.0 * 100.0))
        );

        // The contain range runs from 270 to 290, and the entry range from 220 to 270.
        let interval = timeline
            .state_for(&[edge(2, 0.0, 0.0), edge(3, 0.0, 50.0)])
            .attachment_interval()
            .unwrap();
        assert_eq!(interval.start, TimeValue::percentage((270.0 - 220.0) / 120.0 * 100.0));
        assert_eq!(interval.end, TimeValue::percentage((245.0 - 220.0) / 120.0 * 100.0));

        // Without a box for the subject the timeline is inactive.
        update(&timeline, Some(&metrics), None);
        assert_eq!(timeline.current_time(), None);
    }

    #[test]
    fn the_block_axis_of_a_vertical_writing_mode_is_horizontal() {
        assert_eq!(
            computed_scroll_axis(SCROLL_AXIS_BLOCK, writing_mode::HORIZONTAL_TB, direction::LTR),
            (true, false)
        );
        assert_eq!(
            computed_scroll_axis(SCROLL_AXIS_BLOCK, writing_mode::VERTICAL_RL, direction::LTR),
            (false, true)
        );
        assert_eq!(
            computed_scroll_axis(SCROLL_AXIS_INLINE, writing_mode::HORIZONTAL_TB, direction::RTL),
            (false, true)
        );
        assert_eq!(
            computed_scroll_axis(SCROLL_AXIS_Y, writing_mode::VERTICAL_LR, direction::RTL),
            (true, false)
        );
    }
}
