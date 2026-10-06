/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/// A face of the family being matched, as the font matching algorithm weighs it: its weight range,
/// its slope, and its width as one of the standard width buckets.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFontMatchingCandidate {
    pub weight_min: i32,
    pub weight_max: i32,
    pub slope: i32,
    pub width: u32,
}

/// Writes the order in which to try a family's faces for the desired weight, width and slope to
/// `order`, by index, and returns how many it wrote: a partial implementation of
/// https://drafts.csswg.org/css-fonts-4/#font-matching-algorithm. The first face that renders is the
/// match.
fn font_matching_order(
    candidates: &[FfiFontMatchingCandidate],
    weight: i32,
    width: u32,
    slope: i32,
    order: &mut [usize],
) -> usize {
    let mut matching: Vec<usize> = (0..candidates.len()).collect();
    let mut narrow_to = |matches: &dyn Fn(&FfiFontMatchingCandidate) -> bool| {
        if matching.iter().any(|&index| matches(&candidates[index])) {
            matching.retain(|&index| matches(&candidates[index]));
        }
    };

    // 1. font-width is tried first.
    narrow_to(&|candidate| candidate.width == width);
    // 2. font-style is tried next.
    // We don't have complete support of italic and oblique fonts, so matching on font-style can be simplified to:
    // If a matching slope is found, all faces which don't have that matching slope are excluded from the matching set.
    narrow_to(&|candidate| candidate.slope == slope);
    matching.sort_by_key(|&index| candidates[index].weight_min);

    // The faces from the lightest one that matches on, and up to the heaviest one that does.
    let first = |matches: fn(&FfiFontMatchingCandidate, i32) -> bool| {
        matching
            .iter()
            .position(|&index| matches(&candidates[index], weight))
            .unwrap_or(matching.len())
    };
    let last = |matches: fn(&FfiFontMatchingCandidate, i32) -> bool| {
        matching
            .iter()
            .rposition(|&index| matches(&candidates[index], weight))
            .map_or(0, |last| last + 1)
    };
    let mut length = 0;
    let mut try_faces = |faces: &mut dyn Iterator<Item = &usize>| {
        for &index in faces {
            // A face that did not render the first time it was tried does not render later either.
            if !order[..length].contains(&index) {
                order[length] = index;
                length += 1;
            }
        }
    };

    // 3. font-weight is matched next.
    // If the matching set after performing the steps above includes faces with weight values containing the
    // font-weight desired value, faces with weight values which do not include the desired font-weight value are
    // removed from the matching set.
    // FIXME: This tries every face from the first one that contains the desired value instead.
    try_faces(&mut matching[first(|face, weight| (face.weight_min..=face.weight_max).contains(&weight))..].iter());

    // If there is no face which contains the desired value, a weight value is chosen using the rules below:
    if (400..=500).contains(&weight) {
        // - If the desired weight is inclusively between 400 and 500, weights greater than or equal to the target
        //   weight are checked in ascending order until 500 is hit and checked, followed by weights less than the
        //   target weight in descending order, followed by weights greater than 500, until a match is found.
        let heavier = first(|face, weight| face.weight_min >= weight);
        let above_500 = heavier.max(first(|face, _| face.weight_min > 500));
        try_faces(&mut matching[heavier..above_500].iter());
        try_faces(&mut matching[..last(|face, weight| face.weight_max < weight)].iter().rev());
        try_faces(&mut matching[above_500..].iter());
    } else if weight < 400 {
        // - If the desired weight is less than 400, weights less than or equal to the desired weight are checked in
        //   descending order followed by weights above the desired weight in ascending order until a match is found.
        try_faces(&mut matching[..last(|face, weight| face.weight_max <= weight)].iter().rev());
        try_faces(&mut matching[first(|face, weight| face.weight_min > weight)..].iter());
    } else {
        // - If the desired weight is greater than 500, weights greater than or equal to the desired weight are
        //   checked in ascending order followed by weights below the desired weight in descending order until a
        //   match is found.
        try_faces(&mut matching[first(|face, weight| face.weight_min >= weight)..].iter());
        try_faces(&mut matching[..last(|face, weight| face.weight_max < weight)].iter().rev());
    }
    length
}

/// # Safety
///
/// `candidates` and `order` must point to `count` readable and writable elements respectively, and
/// `count` must not be zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_matching_order(
    candidates: *const FfiFontMatchingCandidate,
    count: usize,
    weight: i32,
    width: u32,
    slope: i32,
    order: *mut usize,
) -> usize {
    // SAFETY: Guaranteed by the caller.
    let (candidates, order) = unsafe {
        (
            std::slice::from_raw_parts(candidates, count),
            std::slice::from_raw_parts_mut(order, count),
        )
    };
    font_matching_order(candidates, weight, width, slope, order)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn weights_are_tried_in_the_order_the_spec_gives() {
        let faces = [100, 300, 500, 700, 900].map(|weight| FfiFontMatchingCandidate {
            weight_min: weight,
            weight_max: weight,
            slope: 0,
            width: 5,
        });
        let order = |weight| {
            let mut order = [0; 5];
            assert_eq!(font_matching_order(&faces, weight, 5, 0, &mut order), 5);
            order
        };
        assert_eq!(order(400), [2, 1, 0, 3, 4]);
        assert_eq!(order(350), [1, 0, 2, 3, 4]);
        assert_eq!(order(600), [3, 4, 2, 1, 0]);
        assert_eq!(order(700), [3, 4, 2, 1, 0]);
    }
}
