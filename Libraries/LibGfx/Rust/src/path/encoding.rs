/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The byte form in which paths cross process boundaries and live in display lists.
//!
//! All values are little-endian: a u8 version, a u8 fill rule, two zero bytes, then the u32 counts of
//! verbs, points and conic weights, then the verbs as u8, zeros up to a multiple of four bytes, the
//! points as pairs of f32, and the conic weights as f32. A path without geometry is written as an
//! empty path.

use super::geometry::{PathGeometry, PathVerb};
use crate::geometry::FloatPoint;
use crate::paint_enums::WindingRule;

const VERSION: u8 = 1;
const HEADER_SIZE: usize = 16;

fn padded_to_four(size: usize) -> usize {
    size.div_ceil(4) * 4
}

pub fn encode(geometry: &PathGeometry) -> Vec<u8> {
    let empty = PathGeometry::new();
    let geometry = if geometry.has_geometry() { geometry } else { &empty };
    let verbs = geometry.verbs();
    let points = geometry.points();
    let conic_weights = geometry.conic_weights();

    let mut bytes =
        Vec::with_capacity(HEADER_SIZE + padded_to_four(verbs.len()) + points.len() * 8 + conic_weights.len() * 4);
    bytes.extend_from_slice(&[VERSION, geometry.fill_rule() as u8, 0, 0]);
    for count in [verbs.len(), points.len(), conic_weights.len()] {
        bytes.extend_from_slice(&(count as u32).to_le_bytes());
    }
    bytes.extend(verbs.iter().map(|verb| *verb as u8));
    bytes.resize(HEADER_SIZE + padded_to_four(verbs.len()), 0);
    for point in points {
        bytes.extend_from_slice(&point.x.to_le_bytes());
        bytes.extend_from_slice(&point.y.to_le_bytes());
    }
    for weight in conic_weights {
        bytes.extend_from_slice(&weight.to_le_bytes());
    }
    bytes
}

/// The path that the bytes describe, or None if they do not describe one: if the sizes do not match
/// the counts, if a contour does not start with a move, if the verbs need other numbers of points or
/// weights than there are, or if a point or weight is not finite or a weight is not positive.
pub fn decode(bytes: &[u8]) -> Option<PathGeometry> {
    let header = bytes.get(..HEADER_SIZE)?;
    if header[0] != VERSION || header[2] != 0 || header[3] != 0 {
        return None;
    }
    let fill_rule = match header[1] {
        0 => WindingRule::Nonzero,
        1 => WindingRule::EvenOdd,
        _ => return None,
    };
    let read_count =
        |offset: usize| u32::from_le_bytes(header[offset..offset + 4].try_into().expect("four bytes")) as usize;
    let (verb_count, point_count, conic_weight_count) = (read_count(4), read_count(8), read_count(12));

    let verbs_end = HEADER_SIZE.checked_add(padded_to_four(verb_count))?;
    let points_end = verbs_end.checked_add(point_count.checked_mul(8)?)?;
    let weights_end = points_end.checked_add(conic_weight_count.checked_mul(4)?)?;
    if bytes.len() != weights_end {
        return None;
    }

    let mut verbs = Vec::with_capacity(verb_count);
    let mut needed_points = 0usize;
    let mut needed_conic_weights = 0usize;
    for &byte in &bytes[HEADER_SIZE..HEADER_SIZE + verb_count] {
        let verb = PathVerb::from_u8(byte)?;
        let starts_contour = matches!(verbs.last(), None | Some(PathVerb::Close));
        if starts_contour && verb != PathVerb::Move {
            return None;
        }
        needed_points += verb.stored_point_count();
        if verb == PathVerb::Conic {
            needed_conic_weights += 1;
        }
        verbs.push(verb);
    }
    if bytes[HEADER_SIZE + verb_count..verbs_end].iter().any(|byte| *byte != 0) {
        return None;
    }
    if needed_points != point_count || needed_conic_weights != conic_weight_count {
        return None;
    }

    let read_f32 = |offset: usize| f32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("four bytes"));
    let points: Vec<FloatPoint> = (0..point_count)
        .map(|index| {
            let offset = verbs_end + index * 8;
            FloatPoint {
                x: read_f32(offset),
                y: read_f32(offset + 4),
            }
        })
        .collect();
    let conic_weights: Vec<f32> = (0..conic_weight_count)
        .map(|index| read_f32(points_end + index * 4))
        .collect();
    if !points.iter().all(|point| point.x.is_finite() && point.y.is_finite())
        || !conic_weights.iter().all(|weight| weight.is_finite() && *weight > 0.0)
    {
        return None;
    }
    Some(PathGeometry::from_checked_contours(
        verbs,
        points,
        conic_weights,
        fill_rule,
    ))
}
