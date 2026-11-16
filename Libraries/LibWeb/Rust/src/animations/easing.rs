/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Easing functions as animations hold them.

use crate::css::easing::{Easing, FfiEasingDescriptor, FfiEasingKind, FfiLinearEasingPoint};

/// The easing keywords that name cubic Bézier curves, with the control points of each.
/// https://drafts.csswg.org/css-easing-1/#cubic-bezier-easing-functions
pub(crate) const KEYWORD_CUBIC_BEZIERS: [(u16, [f64; 4]); 4] = {
    use crate::css::css_enums::keyword;
    [
        (keyword::EASE, [0.25, 0.1, 0.25, 1.0]),
        (keyword::EASE_IN, [0.42, 0.0, 1.0, 1.0]),
        (keyword::EASE_OUT, [0.0, 0.0, 0.58, 1.0]),
        (keyword::EASE_IN_OUT, [0.42, 0.0, 0.58, 1.0]),
    ]
};

/// The cubic Bézier curve of the keyword at `index` of [`KEYWORD_CUBIC_BEZIERS`].
pub(crate) const fn keyword_cubic_bezier(index: usize) -> Easing {
    let [x1, y1, x2, y2] = KEYWORD_CUBIC_BEZIERS[index].1;
    Easing::CubicBezier { x1, y1, x2, y2 }
}

/// The control points of `linear`.
static LINEAR_POINTS: [FfiLinearEasingPoint; 2] = [
    FfiLinearEasingPoint {
        input: 0.0,
        output: 0.0,
    },
    FfiLinearEasingPoint {
        input: 1.0,
        output: 1.0,
    },
];

/// An easing function, held without an allocation of its own where it is `linear` or one of the easings the other
/// keywords name.
#[derive(Clone, PartialEq)]
pub(crate) enum CompactEasing {
    Linear,
    /// The cubic Bézier curve of the keyword at this index of [`KEYWORD_CUBIC_BEZIERS`].
    Keyword(u8),
    Other(Box<Easing>),
}

impl CompactEasing {
    pub(crate) fn of(easing: Easing) -> Self {
        if easing == Easing::default() {
            return Self::Linear;
        }
        let keyword = match &easing {
            &Easing::CubicBezier { x1, y1, x2, y2 } => KEYWORD_CUBIC_BEZIERS
                .iter()
                .position(|(_, control_points)| *control_points == [x1, y1, x2, y2]),
            Easing::Linear(_) | Easing::Steps { .. } => None,
        };
        match keyword {
            #[expect(clippy::cast_possible_truncation)]
            Some(index) => Self::Keyword(index as u8),
            None => Self::Other(Box::new(easing)),
        }
    }

    /// The function, which a keyword's easing lends from a table of them.
    #[must_use]
    pub(crate) fn as_easing(&self) -> &Easing {
        /// `linear`, as `Easing::default()` builds it.
        static LINEAR: std::sync::LazyLock<Easing> = std::sync::LazyLock::new(Easing::default);
        /// The cubic Bézier curves of [`KEYWORD_CUBIC_BEZIERS`], in order.
        static KEYWORD_EASINGS: [Easing; KEYWORD_CUBIC_BEZIERS.len()] = [
            keyword_cubic_bezier(0),
            keyword_cubic_bezier(1),
            keyword_cubic_bezier(2),
            keyword_cubic_bezier(3),
        ];
        match self {
            Self::Linear => &LINEAR,
            Self::Keyword(index) => &KEYWORD_EASINGS[usize::from(*index)],
            Self::Other(easing) => easing,
        }
    }

    #[must_use]
    pub(crate) fn easing(&self) -> Easing {
        match self {
            Self::Linear => Easing::default(),
            Self::Keyword(index) => keyword_cubic_bezier(usize::from(*index)),
            Self::Other(easing) => Easing::clone(easing),
        }
    }

    /// The descriptor of the function, which borrows its control points.
    #[must_use]
    pub(crate) fn descriptor(&self) -> FfiEasingDescriptor {
        match self {
            Self::Linear => FfiEasingDescriptor {
                kind: FfiEasingKind::Linear,
                linear_points: LINEAR_POINTS.as_ptr(),
                linear_point_count: LINEAR_POINTS.len(),
                x1: 0.0,
                y1: 0.0,
                x2: 0.0,
                y2: 0.0,
                interval_count: 0,
                step_position: 0,
            },
            Self::Keyword(index) => keyword_cubic_bezier(usize::from(*index)).descriptor(),
            Self::Other(easing) => easing.descriptor(),
        }
    }

    pub(crate) fn heap_size(&self) -> usize {
        match self {
            Self::Linear | Self::Keyword(_) => 0,
            Self::Other(easing) => {
                size_of::<Easing>()
                    + match &**easing {
                        Easing::Linear(points) => crate::heap_size::vec_bytes(points),
                        Easing::CubicBezier { .. } | Easing::Steps { .. } => 0,
                    }
            }
        }
    }
}
