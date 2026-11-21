/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libgfx_rust::IntRect;
use std::cmp::Ordering;
use std::collections::BTreeMap;

pub const TILE_SIZE_IN_DEVICE_PIXELS: i32 = 512;
const MAXIMUM_CELLS_CONTENT_IS_FOLLOWED_INTO_ONE_BY_ONE: u64 = 4096;

/// A cell of the tile grid of a layer's raster space, which starts at the space's origin.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct TileCoord {
    pub column: i32,
    pub row: i32,
}

impl TileCoord {
    pub const fn new(column: i32, row: i32) -> Self {
        Self { column, row }
    }

    /// The cell that holds the device pixel at `x` and `y`.
    pub const fn of_pixel(x: i32, y: i32) -> Self {
        Self::new(
            x.div_euclid(TILE_SIZE_IN_DEVICE_PIXELS),
            y.div_euclid(TILE_SIZE_IN_DEVICE_PIXELS),
        )
    }

    pub const fn cell_rect(self) -> IntRect {
        IntRect::new(
            self.column * TILE_SIZE_IN_DEVICE_PIXELS,
            self.row * TILE_SIZE_IN_DEVICE_PIXELS,
            TILE_SIZE_IN_DEVICE_PIXELS,
            TILE_SIZE_IN_DEVICE_PIXELS,
        )
    }
}

impl Ord for TileCoord {
    fn cmp(&self, other: &Self) -> Ordering {
        (self.row, self.column).cmp(&(other.row, other.column))
    }
}

impl PartialOrd for TileCoord {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        Some(self.cmp(other))
    }
}

impl std::fmt::Display for TileCoord {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(formatter, "({},{})", self.column, self.row)
    }
}

/// The cells from a first to a last column and from a first to a last row. It holds no cell where a last column or row
/// comes before the first.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct CellRange {
    pub first_row: i32,
    pub last_row: i32,
    pub first_column: i32,
    pub last_column: i32,
}

impl Default for CellRange {
    fn default() -> Self {
        Self::NONE
    }
}

impl CellRange {
    pub const NONE: Self = Self {
        first_row: 0,
        last_row: -1,
        first_column: 0,
        last_column: -1,
    };

    /// The cells that `rect` reaches into.
    pub fn touched_by(rect: IntRect) -> Self {
        if rect.is_empty() {
            return Self::NONE;
        }
        let first = TileCoord::of_pixel(rect.x, rect.y);
        let last = TileCoord::of_pixel(rect.right() - 1, rect.bottom() - 1);
        Self {
            first_row: first.row,
            last_row: last.row,
            first_column: first.column,
            last_column: last.column,
        }
    }

    /// The cells at most `distance` columns and `distance` rows away from `center`.
    pub fn around(center: TileCoord, distance: i32) -> Self {
        Self {
            first_row: center.row.saturating_sub(distance),
            last_row: center.row.saturating_add(distance),
            first_column: center.column.saturating_sub(distance),
            last_column: center.column.saturating_add(distance),
        }
    }

    pub fn is_empty(self) -> bool {
        self.last_row < self.first_row || self.last_column < self.first_column
    }

    /// The rect its cells cover, which is empty where it holds no cell.
    pub fn rect(self) -> IntRect {
        if self.is_empty() {
            return IntRect::default();
        }
        TileCoord::new(self.first_column, self.first_row)
            .cell_rect()
            .united(TileCoord::new(self.last_column, self.last_row).cell_rect())
    }

    pub fn row_count(self) -> u64 {
        if self.is_empty() {
            return 0;
        }
        (i64::from(self.last_row) - i64::from(self.first_row) + 1) as u64
    }

    pub fn cell_count(self) -> u64 {
        if self.is_empty() {
            return 0;
        }
        self.row_count() * (i64::from(self.last_column) - i64::from(self.first_column) + 1) as u64
    }

    pub fn holds_every_cell_of(self, other: Self) -> bool {
        other.is_empty()
            || (self.first_row <= other.first_row
                && other.last_row <= self.last_row
                && self.first_column <= other.first_column
                && other.last_column <= self.last_column)
    }

    pub fn contains(self, coord: TileCoord) -> bool {
        (self.first_row..=self.last_row).contains(&coord.row)
            && (self.first_column..=self.last_column).contains(&coord.column)
    }

    pub fn contains_range(self, other: Self) -> bool {
        other.is_empty()
            || (self.first_row <= other.first_row
                && other.last_row <= self.last_row
                && self.first_column <= other.first_column
                && other.last_column <= self.last_column)
    }

    pub fn intersected(self, other: Self) -> Self {
        let intersection = Self {
            first_row: self.first_row.max(other.first_row),
            last_row: self.last_row.min(other.last_row),
            first_column: self.first_column.max(other.first_column),
            last_column: self.last_column.min(other.last_column),
        };
        if intersection.is_empty() {
            return Self::NONE;
        }
        intersection
    }

    /// Its cells, row by row.
    pub fn coords(self) -> impl Iterator<Item = TileCoord> {
        let columns = self.first_column..=self.last_column;
        (self.first_row..=self.last_row)
            .flat_map(move |row| columns.clone().map(move |column| TileCoord::new(column, row)))
    }
}

/// The cells that `rect` reaches into, row by row. A caller bounds `rect` first, since a rect far larger than the
/// viewport reaches into a great many cells; to find what a map holds within a rect of any size, it uses
/// `entries_within` or `for_each_entry_within`.
pub fn tile_coords_touching(rect: IntRect) -> impl Iterator<Item = TileCoord> {
    CellRange::touched_by(rect).coords()
}

/// Whether content within `rect` reaches too many cells to be followed into each of them, and is taken to reach every
/// cell it can instead.
pub fn reaches_too_many_cells_to_follow_one_by_one(rect: IntRect) -> bool {
    tile_count_touching(rect) > MAXIMUM_CELLS_CONTENT_IS_FOLLOWED_INTO_ONE_BY_ONE
}

/// How many cells `rect` reaches into.
pub fn tile_count_touching(rect: IntRect) -> u64 {
    CellRange::touched_by(rect).cell_count()
}

// A map is looked up row by row where the range has no more rows than the map has entries, and walked whole otherwise,
// so that a range of many more cells than the map holds costs no more than the map.
fn looks_up_rows_of_range<V>(cells: &BTreeMap<TileCoord, V>, range: CellRange) -> bool {
    range.row_count() <= cells.len() as u64
}

fn first_and_last_coord_of_row(range: CellRange, row: i32) -> std::ops::RangeInclusive<TileCoord> {
    TileCoord::new(range.first_column, row)..=TileCoord::new(range.last_column, row)
}

/// The entries of `cells` within `range`, row by row. It costs no more than the rows of `range` or the entries of
/// `cells`, whichever are fewer, and not the cells of `range`.
pub fn entries_within<V>(cells: &BTreeMap<TileCoord, V>, range: CellRange) -> impl Iterator<Item = (&TileCoord, &V)> {
    let looks_up_rows = looks_up_rows_of_range(cells, range);
    let rows_looked_up = (looks_up_rows && !range.is_empty()).then(|| {
        (range.first_row..=range.last_row).flat_map(move |row| cells.range(first_and_last_coord_of_row(range, row)))
    });
    let entries_walked = (!looks_up_rows).then(|| cells.iter().filter(move |(coord, _)| range.contains(**coord)));
    rows_looked_up
        .into_iter()
        .flatten()
        .chain(entries_walked.into_iter().flatten())
}

/// Visits the entries of `cells` within `range`, row by row, at the cost `entries_within` has.
pub fn for_each_entry_within<V>(
    cells: &mut BTreeMap<TileCoord, V>,
    range: CellRange,
    mut visit: impl FnMut(TileCoord, &mut V),
) {
    if range.is_empty() {
        return;
    }
    if !looks_up_rows_of_range(cells, range) {
        for (coord, value) in cells.iter_mut().filter(|(coord, _)| range.contains(**coord)) {
            visit(*coord, value);
        }
        return;
    }
    for row in range.first_row..=range.last_row {
        for (coord, value) in cells.range_mut(first_and_last_coord_of_row(range, row)) {
            visit(*coord, value);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_rect_touches_the_cells_it_reaches_into_row_by_row() {
        let coords: Vec<TileCoord> = tile_coords_touching(IntRect::new(
            TILE_SIZE_IN_DEVICE_PIXELS - 6,
            10,
            TILE_SIZE_IN_DEVICE_PIXELS + 44,
            TILE_SIZE_IN_DEVICE_PIXELS - 6,
        ))
        .collect();
        assert_eq!(
            coords,
            [
                TileCoord::new(0, 0),
                TileCoord::new(1, 0),
                TileCoord::new(2, 0),
                TileCoord::new(0, 1),
                TileCoord::new(1, 1),
                TileCoord::new(2, 1),
            ]
        );
    }

    #[test]
    fn a_rect_ending_on_a_cell_edge_does_not_touch_the_next_cell() {
        let coords: Vec<TileCoord> = tile_coords_touching(IntRect::new(
            0,
            0,
            TILE_SIZE_IN_DEVICE_PIXELS,
            2 * TILE_SIZE_IN_DEVICE_PIXELS,
        ))
        .collect();
        assert_eq!(coords, [TileCoord::new(0, 0), TileCoord::new(0, 1)]);
    }

    #[test]
    fn cells_left_of_and_above_the_origin_have_negative_coords() {
        let coords: Vec<TileCoord> =
            tile_coords_touching(IntRect::new(-1, -TILE_SIZE_IN_DEVICE_PIXELS - 1, 2, 1)).collect();
        assert_eq!(coords, [TileCoord::new(-1, -2), TileCoord::new(0, -2)]);
    }

    #[test]
    fn an_empty_rect_touches_no_cell() {
        assert_eq!(tile_coords_touching(IntRect::new(10, 10, 0, 20)).count(), 0);
        assert!(CellRange::touched_by(IntRect::new(10, 10, 0, 20)).is_empty());
        assert_eq!(CellRange::default(), CellRange::NONE);
    }

    #[test]
    fn the_entries_within_a_range_are_alike_whether_rows_are_looked_up_or_every_entry_is_walked() {
        let held: BTreeMap<TileCoord, u32> = [(-3, -2), (0, 0), (1, 0), (5, 0), (0, 1), (2, 7), (9, 9)]
            .into_iter()
            .enumerate()
            .map(|(index, (column, row))| (TileCoord::new(column, row), index as u32))
            .collect();
        let ranges = [
            CellRange::touched_by(IntRect::new(
                0,
                0,
                2 * TILE_SIZE_IN_DEVICE_PIXELS,
                2 * TILE_SIZE_IN_DEVICE_PIXELS,
            )),
            CellRange::around(TileCoord::new(0, 0), 3),
            CellRange::around(TileCoord::new(4, 4), 1 << 20),
            CellRange::touched_by(IntRect::new(-(1 << 28), -(1 << 28), 1 << 29, 1 << 29)),
            CellRange::NONE,
        ];
        for range in ranges {
            let expected: Vec<(TileCoord, u32)> = held
                .iter()
                .filter(|(coord, _)| range.contains(**coord))
                .map(|(coord, value)| (*coord, *value))
                .collect();
            let found: Vec<(TileCoord, u32)> = entries_within(&held, range)
                .map(|(coord, value)| (*coord, *value))
                .collect();
            assert_eq!(found, expected);
            let mut visited = Vec::new();
            for_each_entry_within(&mut held.clone(), range, |coord, value| visited.push((coord, *value)));
            assert_eq!(visited, expected);
        }
    }

    #[test]
    fn a_window_around_a_cell_holds_the_cells_at_most_its_distance_away() {
        let window = CellRange::around(TileCoord::new(10, -4), 2);
        assert_eq!(window.cell_count(), 25);
        assert!(window.contains(TileCoord::new(8, -6)) && window.contains(TileCoord::new(12, -2)));
        assert!(!window.contains(TileCoord::new(13, -4)));
        assert!(window.contains_range(CellRange::around(TileCoord::new(11, -3), 1)));
        assert!(!window.contains_range(CellRange::around(TileCoord::new(12, -4), 1)));
        assert_eq!(
            window.intersected(CellRange::around(TileCoord::new(13, -4), 1)),
            CellRange::touched_by(IntRect::new(
                12 * TILE_SIZE_IN_DEVICE_PIXELS,
                -5 * TILE_SIZE_IN_DEVICE_PIXELS,
                TILE_SIZE_IN_DEVICE_PIXELS,
                3 * TILE_SIZE_IN_DEVICE_PIXELS
            ))
        );
        assert_eq!(
            window.intersected(CellRange::around(TileCoord::new(20, 0), 1)),
            CellRange::NONE
        );
    }
}
