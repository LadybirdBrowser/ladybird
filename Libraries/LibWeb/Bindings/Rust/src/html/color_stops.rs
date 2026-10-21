/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The color stops of a CanvasGradient, kept in the order of their positions. Stops at the same position stay in
//! the order they were added in, as the spec says.

use libgfx_rust::Color;

/// The color stops, as the parallel arrays of colors and positions that a paint lends the recorder.
#[derive(Debug, Default)]
pub(crate) struct ColorStopList {
    colors: Vec<Color>,
    positions: Vec<f32>,
}

impl ColorStopList {
    pub(crate) fn colors(&self) -> &[Color] {
        &self.colors
    }

    pub(crate) fn positions(&self) -> &[f32] {
        &self.positions
    }

    /// Places the stop after every stop at a position that is not greater than its own.
    pub(crate) fn add(&mut self, position: f32, color: Color) {
        let index = self.positions.partition_point(|&other| other <= position);
        self.positions.insert(index, position);
        self.colors.insert(index, color);
    }
}

#[cfg(test)]
mod tests {
    use super::ColorStopList;
    use libgfx_rust::Color;

    #[test]
    fn stops_are_sorted_by_position() {
        let mut stops = ColorStopList::default();
        stops.add(0.5, Color(1));
        stops.add(0.0, Color(2));
        stops.add(1.0, Color(3));
        stops.add(0.25, Color(4));
        assert_eq!(stops.positions(), [0.0, 0.25, 0.5, 1.0]);
        assert_eq!(stops.colors(), [Color(2), Color(4), Color(1), Color(3)]);
    }

    #[test]
    fn stops_at_one_position_stay_in_the_order_they_were_added() {
        let mut stops = ColorStopList::default();
        for color in 0..20 {
            stops.add(0.5, Color(color));
        }
        assert_eq!(stops.colors(), (0..20).map(Color).collect::<Vec<_>>());
    }

    #[test]
    fn stops_at_several_positions_stay_in_the_order_they_were_added() {
        let mut stops = ColorStopList::default();
        let positions = [0.5, 0.1, 0.5, 0.9, 0.5, 0.1, 0.5, 0.3, 0.5, 0.1, 0.7, 0.5];
        for (color, position) in (0..).zip(positions) {
            stops.add(position, Color(color));
        }
        assert_eq!(
            stops.positions(),
            [0.1, 0.1, 0.1, 0.3, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.7, 0.9]
        );
        assert_eq!(stops.colors(), [1, 5, 9, 7, 0, 2, 4, 6, 8, 11, 10, 3].map(Color));
    }
}
