/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::style::fast_hash::FastMap;
use crate::layout::node_data::NodeSlotId;
use libgfx_rust::WindingRule;
use libgfx_rust::path::{OwnedPath, PathBuilder};
use std::cell::RefCell;
use std::sync::Arc;

/// The state an `<area>`'s `shape` attribute represents, as the HTML image map processing model
/// enumerates it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AreaShape {
    Circle,
    Default,
    Polygon,
    Rectangle,
}

impl AreaShape {
    /// The shape state `HTMLAreaElement::ShapeState` names, whose values the document asserts.
    pub fn from_raw(value: u8) -> Self {
        match value {
            0 => Self::Circle,
            1 => Self::Default,
            2 => Self::Polygon,
            3 => Self::Rectangle,
            _ => panic!("{value} is not an area shape state"),
        }
    }
}

/// What an `<area>`'s shape covers of the image it is layered onto, built once as it is published
/// rather than at every hit.
pub enum AreaCoverage {
    /// The default state, which covers the whole image.
    WholeImage,
    Shape(OwnedPath),
    /// A shape that is empty, such as one with too few coordinates.
    Nothing,
}

impl AreaCoverage {
    pub fn new(shape: AreaShape, coords: &[f64]) -> Self {
        if shape == AreaShape::Default {
            return Self::WholeImage;
        }
        shape_path(shape, coords).map_or(Self::Nothing, Self::Shape)
    }

    fn contains_point(&self, x: f32, y: f32) -> bool {
        match self {
            // AD-HOC: The coordinates of a shape are interpreted relative to the displayed image, so the rectangle
            //         that exactly covers the entire image excludes the image's borders and padding. Treat the default
            //         state as covering everything that hits the image instead.
            Self::WholeImage => true,
            Self::Shape(path) => path.contains(x, y, WindingRule::EvenOdd as i32),
            Self::Nothing => false,
        }
    }
}

/// One `<area>` of an image map, as the image's row holds it: the style-tree identity to name as
/// the hit target, and what its shape covers.
pub struct PublishedImageMapArea {
    pub style_node: u32,
    pub coverage: AreaCoverage,
}

// https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
/// The shape to layer onto the image, built with the same operations the DOM side built it with,
/// so that the containment test is the one `Gfx::Path` already answered.
fn shape_path(shape: AreaShape, coords: &[f64]) -> Option<OwnedPath> {
    // Each area element in areas must be processed as follows to obtain a shape to layer onto the image:

    // 1. Find the state that the element's shape attribute represents.
    // 2. Use the rules for parsing a list of floating-point numbers to parse the element's coords attribute, if it
    //    is present, and let the coords list be the result. If the attribute is absent, let the coords list be the
    //    empty list.
    // NB: The document did both of these before it published the area.
    let vertex = |index: usize| (coords[2 * index] as f32, coords[2 * index + 1] as f32);
    // 3. If the number of items in the coords list is less than the minimum number given for the area element's
    //    current state, as per the following table, then the shape is empty; return.
    // 4. Check for excess items in the coords list as per the entry in the following list corresponding to the
    //    shape attribute's state:
    // NB: Excess items require no handling, because each shape reads only the items it requires.
    // 8. Now, the shape represented by the element is the one described for the entry in the list below
    //    corresponding to the state of the shape attribute:
    match shape {
        AreaShape::Circle => {
            if coords.len() < 3 {
                return None;
            }

            // 7. If the shape attribute represents the circle state, and the third number in the list is less than or
            //    equal to zero, then the shape is empty; return.
            if coords[2] <= 0.0 {
                return None;
            }

            // Let x be the first number in coords, y be the second number, and r be the third number.
            // The shape is a circle whose center is x CSS pixels from the left edge of the image and y CSS pixels from
            // the top edge of the image, and whose radius is r CSS pixels.
            let x = coords[0] as f32;
            let y = coords[1] as f32;
            let radius = coords[2] as f32;
            let mut builder = PathBuilder::new();
            builder.move_to(x + radius, y);
            builder.arc_to(x, y + radius, radius, false, true);
            builder.arc_to(x - radius, y, radius, false, true);
            builder.arc_to(x, y - radius, radius, false, true);
            builder.arc_to(x + radius, y, radius, false, true);
            builder.close();
            Some(builder.build())
        }
        // The shape is a rectangle that exactly covers the entire image.
        // NB: AreaCoverage answers for the default state without a shape.
        AreaShape::Default => None,
        AreaShape::Polygon => {
            if coords.len() < 6 {
                return None;
            }

            let mut builder = PathBuilder::new();
            let (x, y) = vertex(0);
            builder.move_to(x, y);
            for index in 1..coords.len() / 2 {
                let (x, y) = vertex(index);
                builder.line_to(x, y);
            }
            builder.close();
            Some(builder.build())
        }
        AreaShape::Rectangle => {
            if coords.len() < 4 {
                return None;
            }

            let mut corners = [coords[0], coords[1], coords[2], coords[3]];

            // 5. If the shape attribute represents the rectangle state, and the first number in the list is numerically
            //    greater than the third number in the list, then swap those two numbers around.
            if corners[0] > corners[2] {
                corners.swap(0, 2);
            }

            // 6. If the shape attribute represents the rectangle state, and the second number in the list is
            //    numerically greater than the fourth number in the list, then swap those two numbers around.
            if corners[1] > corners[3] {
                corners.swap(1, 3);
            }

            // The shape is a rectangle whose top-left corner is given by the coordinate (x1, y1) and whose bottom right
            // corner is given by the coordinate (x2, y2), those coordinates being interpreted as CSS pixels from the
            // top left corner of the image.
            let [left, top, right, bottom] = corners.map(|corner| corner as f32);
            let mut builder = PathBuilder::new();
            builder.move_to(left, top);
            builder.line_to(right, top);
            builder.line_to(right, bottom);
            builder.line_to(left, bottom);
            builder.close();
            Some(builder.build())
        }
    }
}

// The `<area>` elements of the image map an image is associated with, as the hit test sees them.
// The association and the areas themselves still live on the DOM, and this column is the copy a
// hit test can read without asking for either. The document republishes it before the next hit test
// whenever the list can have changed, and an image box publishes its own when it takes its style.
//
// Keyed by the image's paintable row, because that is the key the hit test has, and an area is
// named by its style-tree identity, because that is what the hit hands back. A row's id carries the
// generation of the slot it came from, so an entry left behind by a freed row names nothing a live
// row can ask for. An image with no image map has no entry, which is nearly every image.
#[derive(Clone, Default)]
pub struct ImageMapAreaColumn {
    /// Shared with the rows published for the host, so a write copies the maps only while those hold them.
    maps: RefCell<Arc<ImageMaps>>,
}

/// The `<area>` elements of the image map of every image that has one, by the image's paintable row.
pub type ImageMaps = FastMap<NodeSlotId, Arc<[PublishedImageMapArea]>>;

impl ImageMapAreaColumn {
    /// Publishes the `<area>` elements of the image map an image is associated with, in tree order.
    pub fn publish(&self, slot: NodeSlotId, areas: Box<[PublishedImageMapArea]>) {
        if slot.is_invalid() {
            return;
        }
        let mut published = self.maps.borrow_mut();
        if areas.is_empty() {
            if published.contains_key(&slot) {
                Arc::make_mut(&mut published).remove(&slot);
            }
        } else {
            Arc::make_mut(&mut published).insert(slot, areas.into());
        }
    }

    pub fn forget(&self, slot: NodeSlotId) {
        let mut published = self.maps.borrow_mut();
        if published.contains_key(&slot) {
            Arc::make_mut(&mut published).remove(&slot);
        }
    }

    /// Whether the image whose paintable row is `slot` has an image map.
    pub(crate) fn has_areas(&self, slot: NodeSlotId) -> bool {
        self.maps.borrow().contains_key(&slot)
    }

    /// The maps as they are now, for the host to read.
    pub(crate) fn snapshot(&self) -> Arc<ImageMaps> {
        self.maps.borrow().clone()
    }

    /// Where the maps are, which moves when a write copies the ones a snapshot shares.
    pub(crate) fn address(&self) -> usize {
        Arc::as_ptr(&self.maps.borrow()).addr()
    }
}

/// The first area of the map of the image whose paintable row is `slot`, in tree order, whose shape covers the point,
/// named by its style-tree identity. Zero when the image has no map, or no shape covers the point.
pub(crate) fn area_for_point(maps: &ImageMaps, slot: NodeSlotId, x: f32, y: f32) -> u32 {
    let Some(areas) = maps.get(&slot) else {
        return 0;
    };
    // https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
    // Pointing device interaction with an image associated with a set of layered shapes per the above algorithm must
    // result in the relevant user interaction events being first fired to the top-most shape covering the point that
    // the pointing device indicated, if any, or to the image element itself, if there is no shape covering that point.
    // NB: The shapes are layered in reverse tree order, so the top-most shape covering the point belongs to the first
    //     area element in tree order whose shape contains the point.
    areas
        .iter()
        .find(|area| area.coverage.contains_point(x, y))
        .map_or(0, |area| area.style_node)
}
