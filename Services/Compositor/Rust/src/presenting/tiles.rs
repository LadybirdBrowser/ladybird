/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tiles the layers of a context hold, the pool of their pixels, and the surface tiles are rastered through. The
//! pixels of a tile are pooled under the tile's content hash, which hashes all they depend on, so a layer finds the
//! tiles another layer or an earlier stack rastered. A window server that shows tiles shows each caret and scroll bar
//! from a surface of its own, painted anew each time what it draws changes, as a caret blinks or a thumb moves.

use super::layer_overlay::{ModelTile, RasterCause};
use super::layers::LayerCompositingInFrame;
use super::{PresentingEnvironment, inflated, rect_of_size, size_of};
use crate::host::ContextTimer;
use crate::raster_host::{RasterHost, SurfaceBlend, SurfaceSampling};
use crate::surfaces::painting_surface::PaintingSurface;
use crate::surfaces::presentable_surface::PresentableSurface;
use libcompositing_rust::display_list::layers::{BarrierCutsOfCell, LayerIdentity, SpaceToFrame};
use libcompositing_rust::display_list::tile_keys::{ContentHash, TileCoverage};
use libcompositing_rust::display_list::tiles::{
    CellRange, TILE_BORDER_IN_DEVICE_PIXELS, TileCoord, entries_within, with_tile_border,
};
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::{Color, FloatPoint, FloatSize, IntRect, IntSize};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::rc::Rc;

const RASTER_SCRATCH_SURFACE_RELEASE_DELAY: Duration = Duration::from_milliseconds(3000);
const TIME_SPARE_TILE_STAYS_POOLED_FOR_CONTENT_THAT_COMES_BACK: Duration = Duration::from_milliseconds(3000);
const TIME_SPARE_SURFACE_IS_KEPT_FOR_NEXT_TILE: Duration = Duration::from_milliseconds(3000);
// About the tiles of two rows of a wide viewport, which a scroll lets go of and needs anew with each row it passes.
const MAXIMUM_SPARE_SURFACE_COUNT: usize = 16;

pub(super) struct TileShownInFrame {
    pub(super) coord: TileCoord,
    pub(super) content_hash: ContentHash,
    pub(super) part_of_cell_needed: IntRect,
    pub(super) cuts: BarrierCutsOfCell,
    pub(super) raster_margin_in_device_pixels: Option<i32>,
    pub(super) coverage: TileCoverage,
}

struct PooledTile {
    pixels: TilePixels,
    held_part_of_cell: IntRect,
    holds_current_pixels: bool,
    /// Of a tile no layer holds, when a layer last held it.
    last_held_at: MonotonicTime,
    held_at_latest_eviction: bool,
}

enum TilePixels {
    Surface {
        surface: Rc<PresentableSurface>,
        every_pixel_is_opaque: bool,
    },
    OneColor(Color),
}

/// The surfaces of tiles that left the pool, kept for a while for the tiles rastered next, as a surface the window
/// server can show takes long to make. A tile takes one only once the window server no longer reads it.
#[derive(Default)]
struct SpareSurfaces {
    least_recently_spared_first: Vec<(Rc<PresentableSurface>, IntSize, MonotonicTime)>,
}

impl SpareSurfaces {
    fn keep(&mut self, surface: Rc<PresentableSurface>, size: IntSize, now: MonotonicTime) {
        self.least_recently_spared_first.push((surface, size, now));
        if self.least_recently_spared_first.len() > MAXIMUM_SPARE_SURFACE_COUNT {
            self.least_recently_spared_first.remove(0);
        }
    }

    fn keep_surface_of(&mut self, tile: PooledTile, now: MonotonicTime) {
        if let TilePixels::Surface { surface, .. } = tile.pixels {
            self.keep(surface, size_of(with_tile_border(tile.held_part_of_cell)), now);
        }
    }

    fn take(
        &mut self,
        size: IntSize,
        is_usable: impl Fn(&Rc<PresentableSurface>) -> bool,
    ) -> Option<Rc<PresentableSurface>> {
        let position = self
            .least_recently_spared_first
            .iter()
            .position(|(surface, surface_size, _)| *surface_size == size && !surface.is_busy() && is_usable(surface))?;
        Some(self.least_recently_spared_first.remove(position).0)
    }

    /// Drops the surfaces spared too long ago, and answers how long the least recently spared one left is still kept.
    fn drop_surfaces_spared_too_long_ago(&mut self, now: MonotonicTime) -> Option<Duration> {
        self.least_recently_spared_first
            .retain(|(_, _, spared_at)| now - *spared_at < TIME_SPARE_SURFACE_IS_KEPT_FOR_NEXT_TILE);
        self.least_recently_spared_first
            .first()
            .map(|(_, _, spared_at)| TIME_SPARE_SURFACE_IS_KEPT_FOR_NEXT_TILE - (now - *spared_at))
    }
}

impl PooledTile {
    fn holds_current_pixels_over_ignoring_backend(&self, part_of_cell_needed: IntRect) -> bool {
        self.holds_current_pixels && self.held_part_of_cell.contains_rect(part_of_cell_needed)
    }

    fn surface(&self) -> Option<&Rc<PresentableSurface>> {
        match &self.pixels {
            TilePixels::Surface { surface, .. } => Some(surface),
            TilePixels::OneColor(_) => None,
        }
    }

    fn coverage(&self) -> TileCoverage {
        match self.pixels {
            TilePixels::Surface {
                every_pixel_is_opaque: true,
                ..
            } => TileCoverage::Opaque,
            TilePixels::Surface {
                every_pixel_is_opaque: false,
                ..
            } => TileCoverage::Unknown,
            TilePixels::OneColor(color) => TileCoverage::OneColor(color),
        }
    }
}

type TilePool = HashMap<ContentHash, PooledTile>;

/// The tiles a layer holds, by cell.
#[derive(Default)]
struct HeldTiles {
    by_cell: BTreeMap<TileCoord, ContentHash>,
}

fn pooled_held_tiles<'a>(
    held_tiles: &'a BTreeMap<TileCoord, ContentHash>,
    tile_pool: &'a TilePool,
) -> impl Iterator<Item = (TileCoord, &'a PooledTile)> {
    held_tiles
        .iter()
        .filter_map(|(coord, content_hash)| Some((*coord, tile_pool.get(content_hash)?)))
}

pub(super) enum TileDraw<'a> {
    TileSurface {
        coord: TileCoord,
        surface: &'a Rc<PresentableSurface>,
        every_pixel_is_opaque: bool,
        rect_in_layer_space: IntRect,
        /// What of the surface shows at `rect_in_layer_space`. Around it, the surface of a tile holds a border of what
        /// lies beside the tile.
        rect_in_surface: IntRect,
    },
    FillOfJoinedOneColorTiles {
        rect_in_layer_space: IntRect,
        color: Color,
    },
}

/// What rastering the tiles a layer shows needs to know of the layer besides its tiles.
pub(super) struct LayerToRaster<'a> {
    pub(super) identity: LayerIdentity,
    pub(super) shown: &'a [TileShownInFrame],
    pub(super) rect_shown_in_layer_space: IntRect,
    pub(super) kept_tile_rect: IntRect,
    pub(super) clear_color: Color,
    /// What a raster of several tiles at once stays within, for a layer of the viewport's space: the viewport.
    pub(super) rastered_within: Option<IntRect>,
    /// Whether the window server shows the tiles of the layer, which their surfaces then have to let it.
    pub(super) shown_by_window_server: bool,
    /// Whether the window server that shows the context shows tiles, so that the surface of a new tile is one it can
    /// show, whoever shows the layer in this frame.
    pub(super) window_server_shows_tiles: bool,
}

pub(super) struct RasteredLayer {
    /// In the layer's space, where tiles are painted straight from the list into the frame instead.
    pub(super) rects_painted_straight: Vec<IntRect>,
    pub(super) tile_count: u64,
}

/// What the surface of a caret or a scroll bar holds: what its records draw, rastered into `rect` of the layer's space
/// under the raster transform of the frame.
#[derive(Clone, Copy, PartialEq)]
pub(super) struct OwnSurfacePainting {
    pub(super) content_hash: ContentHash,
    pub(super) rect: IntRect,
    pub(super) raster_scale: FloatSize,
    pub(super) raster_translation: FloatPoint,
}

struct HeldOwnSurface {
    surface: Rc<PresentableSurface>,
    painting: OwnSurfacePainting,
}

/// The tiles the layers of a context hold under the identities of the layers, and the pool of their pixels.
#[derive(Default)]
pub(super) struct LayerTiles {
    held_by_layer: HashMap<LayerIdentity, HeldTiles>,
    pool: TilePool,
    /// The surface a window server that shows tiles shows each caret and scroll bar from.
    own_surface_by_layer: HashMap<LayerIdentity, HeldOwnSurface>,
    /// Whether a layer took or let go of tiles, or the pixels of pooled tiles changed, since spare tiles were last
    /// evicted.
    changed_since_eviction: bool,
    raster_scratch_surface: Option<Rc<PaintingSurface>>,
    spare_surfaces: SpareSurfaces,
}

impl LayerTiles {
    /// The coverage of the pooled tile of the content hash, where it holds current pixels over the part of its cell.
    pub(super) fn current_coverage(
        &self,
        content_hash: ContentHash,
        part_of_cell_needed: IntRect,
    ) -> Option<TileCoverage> {
        self.pool
            .get(&content_hash)
            .filter(|pooled| pooled.holds_current_pixels_over_ignoring_backend(part_of_cell_needed))
            .map(PooledTile::coverage)
    }

    pub(super) fn bounds_held_by(&self, identity: &LayerIdentity) -> IntRect {
        let Some(held) = self.held_by_layer.get(identity) else {
            return IntRect::default();
        };
        pooled_held_tiles(&held.by_cell, &self.pool).fold(IntRect::default(), |bounds, (_, tile)| {
            bounds.united(tile.held_part_of_cell)
        })
    }

    pub(super) fn model_tiles_of(&self, identity: &LayerIdentity) -> Vec<ModelTile> {
        let Some(held) = self.held_by_layer.get(identity) else {
            return Vec::new();
        };
        pooled_held_tiles(&held.by_cell, &self.pool)
            .map(|(coord, tile)| ModelTile {
                coord,
                held_part_of_cell: Some(tile.held_part_of_cell),
                is_one_color: tile.surface().is_none(),
                raster_count: 0,
                last_raster: None,
            })
            .collect()
    }

    pub(super) fn stale_rect_of_layer(
        &self,
        shown: &[TileShownInFrame],
        compositing: &LayerCompositingInFrame,
        viewport_rect: IntRect,
        composited_filter_read_reach: i32,
    ) -> IntRect {
        let no_held_tiles = BTreeMap::new();
        let held_tiles = self
            .held_by_layer
            .get(&compositing.identity)
            .map_or(&no_held_tiles, |held| &held.by_cell);
        let rect_shown_in_layer_space =
            compositing.rect_shown_in_layer_space(viewport_rect, composited_filter_read_reach);
        let in_frame_as_far_as_filters_around_layer_read = |rect_in_layer_space: IntRect| {
            compositing.rect_in_frame(
                inflated(rect_in_layer_space, composited_filter_read_reach),
                viewport_rect,
            )
        };
        let mut stale_rect = IntRect::default();
        debug_assert!(shown.is_sorted_by_key(|tile| tile.coord));
        for (coord, held_tile) in pooled_held_tiles(held_tiles, &self.pool) {
            if held_tile_lies_in_shown_rect_at_unshown_cell(coord, held_tile, rect_shown_in_layer_space, shown) {
                stale_rect = stale_rect.united(in_frame_as_far_as_filters_around_layer_read(
                    held_tile.held_part_of_cell,
                ));
            }
        }
        for tile in shown {
            let held_content_hash = held_tiles.get(&tile.coord);
            let tile_is_unchanged = held_content_hash == Some(&tile.content_hash)
                && self
                    .pool
                    .get(&tile.content_hash)
                    .is_some_and(|pooled| pooled.holds_current_pixels_over_ignoring_backend(tile.part_of_cell_needed));
            if tile_is_unchanged {
                continue;
            }
            stale_rect = stale_rect.united(in_frame_as_far_as_filters_around_layer_read(tile.part_of_cell_needed));
            if let Some(held_tile) = held_content_hash.and_then(|held_content_hash| self.pool.get(held_content_hash)) {
                stale_rect = stale_rect.united(in_frame_as_far_as_filters_around_layer_read(
                    held_tile.held_part_of_cell,
                ));
            }
        }
        stale_rect
    }

    /// Per content hash, how many layers hold or show a tile of it in the frame.
    pub(super) fn holder_counts(&self, shown_tiles_by_layer: &[Vec<TileShownInFrame>]) -> HashMap<ContentHash, usize> {
        let mut holder_count_by_content_hash: HashMap<ContentHash, usize> = HashMap::new();
        for content_hash in self.held_by_layer.values().flat_map(|held| held.by_cell.values()) {
            *holder_count_by_content_hash.entry(*content_hash).or_default() += 1;
        }
        for tile in shown_tiles_by_layer.iter().flatten() {
            *holder_count_by_content_hash.entry(tile.content_hash).or_default() += 1;
        }
        holder_count_by_content_hash
    }

    pub(super) fn draws_reaching_into(
        &self,
        compositing: &LayerCompositingInFrame,
        area_in_frame: IntRect,
    ) -> Vec<TileDraw<'_>> {
        let Some(held) = self.held_by_layer.get(&compositing.identity) else {
            return Vec::new();
        };
        let mut draws = Vec::new();
        let mut one_color_fills = Vec::new();
        // A transform can map the area to many more cells than the layer holds tiles at.
        let area_in_layer_space = compositing.space_to_frame.rect_in_space(area_in_frame);
        for (coord, content_hash) in entries_within(&held.by_cell, CellRange::touched_by(area_in_layer_space)) {
            let Some(tile) = self.pool.get(content_hash) else {
                continue;
            };
            if compositing
                .rect_in_frame(tile.held_part_of_cell, area_in_frame)
                .is_empty()
            {
                continue;
            }
            let rect_in_layer_space = tile.held_part_of_cell;
            match &tile.pixels {
                TilePixels::Surface {
                    surface,
                    every_pixel_is_opaque,
                } => draws.push(TileDraw::TileSurface {
                    coord: *coord,
                    surface,
                    every_pixel_is_opaque: *every_pixel_is_opaque,
                    rect_in_layer_space,
                    rect_in_surface: rect_of_size(size_of(rect_in_layer_space))
                        .translated(TILE_BORDER_IN_DEVICE_PIXELS, TILE_BORDER_IN_DEVICE_PIXELS),
                }),
                TilePixels::OneColor(color) => one_color_fills.push((rect_in_layer_space, *color)),
            }
        }
        draws.extend(joined_rects_of_grid_given_row_by_row(one_color_fills).into_iter().map(
            |(rect_in_layer_space, color)| TileDraw::FillOfJoinedOneColorTiles {
                rect_in_layer_space,
                color,
            },
        ));
        draws
    }

    pub(super) fn has_layer(&self, identity: &LayerIdentity) -> bool {
        self.held_by_layer.contains_key(identity)
    }

    pub(super) fn copy_into(
        &self,
        raster: &RasterHost,
        target: &Rc<PaintingSurface>,
        area_in_frame: IntRect,
        compositing: &LayerCompositingInFrame,
        sampling: SurfaceSampling,
    ) {
        let SpaceToFrame::Offset(composite_offset) = compositing.space_to_frame else {
            unreachable!("the frame holds the layers of spaces it composites through a transform");
        };
        let save_count = raster.save(target);
        if let Some(composite_clip) = compositing.composite_clip {
            raster.clip_rect(target, composite_clip);
        }
        for draw in self.draws_reaching_into(compositing, area_in_frame) {
            match draw {
                TileDraw::TileSurface {
                    surface,
                    rect_in_layer_space,
                    rect_in_surface,
                    ..
                } => {
                    raster.draw_surface(
                        target,
                        surface.painting_surface(),
                        rect_in_surface,
                        rect_in_layer_space.translated(composite_offset.x, composite_offset.y),
                        SurfaceBlend::Source,
                        sampling,
                    );
                }
                TileDraw::FillOfJoinedOneColorTiles {
                    rect_in_layer_space,
                    color,
                } => {
                    let save_count = raster.save(target);
                    raster.clip_rect(
                        target,
                        rect_in_layer_space.translated(composite_offset.x, composite_offset.y),
                    );
                    raster.clear(target, color);
                    raster.restore_to_count(target, save_count);
                }
            }
        }
        raster.restore_to_count(target, save_count);
    }

    /// Drops the tiles of the layers that left the stack, whose pixels stay in the pool for a while.
    pub(super) fn keep_only_layers(&mut self, identities: &HashSet<LayerIdentity>) {
        let layer_count_before = self.held_by_layer.len();
        self.held_by_layer.retain(|identity, _| identities.contains(identity));
        self.changed_since_eviction |= self.held_by_layer.len() != layer_count_before;
        self.own_surface_by_layer
            .retain(|identity, _| identities.contains(identity));
    }

    /// Keeps the surface that the window server shows the caret or scroll bar of the layer of `identity` from, and
    /// paints `painting` into it with `paint` where it holds something else, or is on another backend than `like`, as
    /// each time a caret blinks or a thumb moves. Nothing is painted into a surface the window server shows or still
    /// reads, so the layer takes a spare surface and spares the one it held. Answers whether it painted. Where no
    /// surface can be had, the window server shows nothing of the layer.
    pub(super) fn paint_own_surface(
        &mut self,
        env: &PresentingEnvironment<'_>,
        like: &Rc<PaintingSurface>,
        identity: LayerIdentity,
        painting: OwnSurfacePainting,
        paint: impl FnOnce(&Rc<PaintingSurface>),
    ) -> bool {
        let usable = |surface: &Rc<PresentableSurface>| {
            surface.can_be_shown_by_window_server() && surface.painting_surface().shares_backend_with(like)
        };
        if self
            .own_surface_by_layer
            .get(&identity)
            .is_some_and(|held| held.painting == painting && usable(&held.surface))
        {
            return false;
        }
        if let Some(left) = self.own_surface_by_layer.remove(&identity) {
            self.spare_surfaces
                .keep(left.surface, size_of(left.painting.rect), env.now);
            self.changed_since_eviction = true;
        }
        let size = size_of(painting.rect);
        let Some(surface) = self
            .spare_surfaces
            .take(size, usable)
            .or_else(|| create_tile_surface_on_backend_of(env.raster, size, like, true).filter(usable))
        else {
            return false;
        };
        let (raster, target) = (env.raster, surface.painting_surface());
        let save_count = raster.save(target);
        raster.translate(target, -painting.rect.x as f32, -painting.rect.y as f32);
        raster.clip_rect(target, painting.rect);
        raster.clear(target, Color::TRANSPARENT);
        paint(target);
        raster.restore_to_count(target, save_count);
        self.own_surface_by_layer
            .insert(identity, HeldOwnSurface { surface, painting });
        true
    }

    /// The surface the window server shows the caret or scroll bar of the layer of `identity` from, and where it lies in
    /// the layer's space.
    pub(super) fn own_surface_of(&self, identity: &LayerIdentity) -> Option<(&Rc<PresentableSurface>, IntRect)> {
        let held = self.own_surface_by_layer.get(identity)?;
        Some((&held.surface, held.painting.rect))
    }

    /// Evicts spare tiles where the tiles changed since spare tiles were last evicted, as otherwise every tile held then
    /// is still held, and the timer evicts what grows too old.
    pub(super) fn evict_spare_tiles_if_tiles_changed(&mut self, env: &PresentingEnvironment<'_>) {
        if self.changed_since_eviction {
            self.evict_spare_tiles(env);
        }
    }

    pub(super) fn evict_spare_tiles(&mut self, env: &PresentingEnvironment<'_>) {
        if let Some(time_until_a_spare_tile_grows_too_old) = self.evict_spare_tiles_at(env.now) {
            env.host.set_timer(
                env.context_id,
                ContextTimer::SpareTileEviction,
                time_until_a_spare_tile_grows_too_old,
            );
        }
    }

    /// Evicts spare tiles as of `now`, whose surfaces are kept for a while for the tiles rastered next, and answers how
    /// long the least recently held spare tile left stays pooled, or the least recently spared surface left is kept,
    /// whichever is shorter.
    fn evict_spare_tiles_at(&mut self, now: MonotonicTime) -> Option<Duration> {
        let held_content_hashes: HashSet<ContentHash> = self
            .held_by_layer
            .values()
            .flat_map(|held| held.by_cell.values().copied())
            .collect();
        let surface_pixel_count_of = |tile: &PooledTile| match tile.pixels {
            TilePixels::Surface { .. } => {
                tile.held_part_of_cell.width.max(0) as u64 * tile.held_part_of_cell.height.max(0) as u64
            }
            TilePixels::OneColor(_) => 0,
        };
        let mut pixel_count_of_held_tiles = 0u64;
        let mut spare_tiles: Vec<(MonotonicTime, ContentHash, u64)> = Vec::new();
        let mut content_hashes_of_tiles_to_evict: Vec<ContentHash> = Vec::new();
        for (content_hash, tile) in &mut self.pool {
            if held_content_hashes.contains(content_hash) {
                tile.held_at_latest_eviction = true;
                pixel_count_of_held_tiles += surface_pixel_count_of(tile);
                continue;
            }
            // A frame that lets go of tiles evicts spare tiles as it is committed, so a tile let go since the latest
            // eviction was held until now.
            if std::mem::take(&mut tile.held_at_latest_eviction) {
                tile.last_held_at = now;
            }
            if !tile.holds_current_pixels
                || now - tile.last_held_at >= TIME_SPARE_TILE_STAYS_POOLED_FOR_CONTENT_THAT_COMES_BACK
            {
                content_hashes_of_tiles_to_evict.push(*content_hash);
                continue;
            }
            spare_tiles.push((tile.last_held_at, *content_hash, surface_pixel_count_of(tile)));
        }
        self.changed_since_eviction = false;
        let spare_tiles_least_recently_held_first = {
            spare_tiles.sort_unstable();
            spare_tiles
        };
        let spare_pixel_budget = pixel_count_of_held_tiles;
        let mut pixel_count_of_spare_tiles: u64 = spare_tiles_least_recently_held_first
            .iter()
            .map(|(_, _, pixel_count)| pixel_count)
            .sum();
        let mut evicted_count = 0;
        for (_, content_hash, pixel_count) in &spare_tiles_least_recently_held_first {
            if pixel_count_of_spare_tiles <= spare_pixel_budget {
                break;
            }
            content_hashes_of_tiles_to_evict.push(*content_hash);
            pixel_count_of_spare_tiles -= pixel_count;
            evicted_count += 1;
        }
        for content_hash in content_hashes_of_tiles_to_evict {
            if let Some(evicted_tile) = self.pool.remove(&content_hash) {
                self.spare_surfaces.keep_surface_of(evicted_tile, now);
            }
        }
        let time_least_recently_held_spare_tile_stays_pooled = spare_tiles_least_recently_held_first
            .get(evicted_count)
            .map(|(least_recently_held_at, _, _)| {
                TIME_SPARE_TILE_STAYS_POOLED_FOR_CONTENT_THAT_COMES_BACK - (now - *least_recently_held_at)
            });
        let time_least_recently_spared_surface_is_kept = self.spare_surfaces.drop_surfaces_spared_too_long_ago(now);
        [
            time_least_recently_held_spare_tile_stays_pooled,
            time_least_recently_spared_surface_is_kept,
        ]
        .into_iter()
        .flatten()
        .min()
    }

    pub(super) fn release_raster_scratch_surface(&mut self) {
        self.raster_scratch_surface = None;
    }

    /// Keeps the tiles the layer shows and those it may show again soon, and rasters the shown tiles that lack current
    /// pixels: `replay_tiles` replays the records of the tiles it is given into a target, and `record_raster` hears of
    /// each tile rastered or painted straight into the frame.
    pub(super) fn raster_layer(
        &mut self,
        env: &PresentingEnvironment<'_>,
        like: &Rc<PaintingSurface>,
        layer: LayerToRaster<'_>,
        holder_count_by_content_hash: &HashMap<ContentHash, usize>,
        replay_tiles: impl Fn(&Rc<PaintingSurface>, &[&TileShownInFrame]),
        mut record_raster: Option<impl FnMut(TileCoord, RasterCause, IntRect)>,
    ) -> RasteredLayer {
        let LayerToRaster {
            identity,
            shown,
            rect_shown_in_layer_space,
            kept_tile_rect,
            clear_color,
            rastered_within,
            shown_by_window_server,
            window_server_shows_tiles,
        } = layer;
        let raster = env.raster;
        let records_rasters = record_raster.is_some();
        let mut record = |coord: TileCoord, cause: RasterCause, rect: IntRect| {
            if let Some(record_raster) = &mut record_raster {
                record_raster(coord, cause, rect);
            }
        };
        let mut rects_painted_straight = Vec::new();
        let held = self.held_by_layer.entry(identity).or_default();
        let tile_pool = &mut self.pool;
        let spare_surfaces = &mut self.spare_surfaces;
        let surface_can_be_drawn_and_shown = |surface: &Rc<PresentableSurface>| {
            surface.painting_surface().shares_backend_with(like)
                && (!shown_by_window_server || surface.can_be_shown_by_window_server())
        };

        let held_tile_count_before = held.by_cell.len();
        debug_assert!(shown.is_sorted_by_key(|tile| tile.coord));
        held.by_cell.retain(|coord, content_hash| {
            !coord.cell_rect().intersected(kept_tile_rect).is_empty()
                && tile_pool.get(content_hash).is_some_and(|held_tile| {
                    !held_tile_lies_in_shown_rect_at_unshown_cell(*coord, held_tile, rect_shown_in_layer_space, shown)
                })
        });
        let mut held_tiles_changed = held.by_cell.len() != held_tile_count_before;

        let mut tiles_to_raster: Vec<TileToRaster> = Vec::new();
        for (shown_tile_index, tile) in shown.iter().enumerate() {
            let held_content_hash = held.by_cell.get(&tile.coord).copied();
            let raster_cause_for_overlay = records_rasters.then(|| {
                let tile_held_at_cell = held_content_hash.and_then(|held_content_hash| {
                    Some((
                        tile_pool.get(&held_content_hash)?,
                        held_content_hash == tile.content_hash,
                    ))
                });
                raster_cause_of(
                    tile_held_at_cell,
                    tile.part_of_cell_needed,
                    surface_can_be_drawn_and_shown,
                )
            });
            if held_content_hash != Some(tile.content_hash) {
                held.by_cell.insert(tile.coord, tile.content_hash);
                held_tiles_changed = true;
            }
            let pooled_tile_holds_needed_part_drawable_into_frame = |pooled: &PooledTile| {
                pooled.held_part_of_cell.contains_rect(tile.part_of_cell_needed)
                    && pooled.surface().is_none_or(surface_can_be_drawn_and_shown)
            };
            if tile_pool.get(&tile.content_hash).is_some_and(|pooled| {
                pooled_tile_holds_needed_part_drawable_into_frame(pooled) && pooled.holds_current_pixels
            }) {
                continue;
            }
            if let TileCoverage::OneColor(color) = tile.coverage {
                let replaced_tile = tile_pool.insert(
                    tile.content_hash,
                    PooledTile {
                        pixels: TilePixels::OneColor(color),
                        held_part_of_cell: tile.part_of_cell_needed,
                        holds_current_pixels: true,
                        last_held_at: env.now,
                        held_at_latest_eviction: false,
                    },
                );
                if let Some(replaced_tile) = replaced_tile {
                    spare_surfaces.keep_surface_of(replaced_tile, env.now);
                }
                held_tiles_changed = true;
                continue;
            }
            let every_pixel_is_opaque = tile.coverage == TileCoverage::Opaque;
            match tile_pool.get_mut(&tile.content_hash) {
                // Nothing is painted into a surface the window server shows or still reads, as it would show the
                // painting under way, and ahead of the frame the painting is for.
                Some(pooled)
                    if pooled.surface().is_some_and(|surface| !surface.is_busy())
                        && pooled_tile_holds_needed_part_drawable_into_frame(pooled) =>
                {
                    if let TilePixels::Surface {
                        every_pixel_is_opaque: pooled_every_pixel_is_opaque,
                        ..
                    } = &mut pooled.pixels
                    {
                        *pooled_every_pixel_is_opaque = every_pixel_is_opaque;
                    }
                }
                _ => {
                    // The surface of the tile the layer held at the cell takes the new content hash only where it holds
                    // the very part of the cell a new surface would: the GPU antialiases some content, such as rounded
                    // clips, by where in its surface the content lands.
                    let surface_held_by_this_layer_alone = held_content_hash
                        .filter(|held_content_hash| {
                            *held_content_hash != tile.content_hash
                                && holder_count_by_content_hash
                                    .get(held_content_hash)
                                    .copied()
                                    .unwrap_or(0)
                                    <= 1
                                && tile_pool.get(held_content_hash).is_some_and(|held_tile| {
                                    held_tile.held_part_of_cell == tile.part_of_cell_needed
                                        && held_tile.surface().is_some_and(|surface| {
                                            surface_can_be_drawn_and_shown(surface) && !surface.is_busy()
                                        })
                                })
                        })
                        .and_then(|held_content_hash| tile_pool.remove(&held_content_hash))
                        .and_then(|held_tile| match held_tile.pixels {
                            TilePixels::Surface { surface, .. } => Some(surface),
                            TilePixels::OneColor(_) => None,
                        });
                    let surface_size = size_of(with_tile_border(tile.part_of_cell_needed));
                    let surface = surface_held_by_this_layer_alone
                        .or_else(|| spare_surfaces.take(surface_size, surface_can_be_drawn_and_shown))
                        .or_else(|| {
                            create_tile_surface_on_backend_of(raster, surface_size, like, window_server_shows_tiles)
                                .filter(surface_can_be_drawn_and_shown)
                        });
                    let Some(surface) = surface else {
                        held.by_cell.remove(&tile.coord);
                        held_tiles_changed = true;
                        rects_painted_straight.push(tile.part_of_cell_needed);
                        record(
                            tile.coord,
                            RasterCause::PaintedStraightIntoTheFrame,
                            tile.part_of_cell_needed,
                        );
                        continue;
                    };
                    let replaced_tile = tile_pool.insert(
                        tile.content_hash,
                        PooledTile {
                            pixels: TilePixels::Surface {
                                surface,
                                every_pixel_is_opaque,
                            },
                            held_part_of_cell: tile.part_of_cell_needed,
                            holds_current_pixels: false,
                            last_held_at: env.now,
                            held_at_latest_eviction: false,
                        },
                    );
                    if let Some(replaced_tile) = replaced_tile {
                        spare_surfaces.keep_surface_of(replaced_tile, env.now);
                    }
                }
            }
            tiles_to_raster.push(TileToRaster {
                shown_tile_index,
                raster_cause_for_overlay,
                was_rastered: false,
            });
        }
        if held_tiles_changed || !tiles_to_raster.is_empty() {
            self.changed_since_eviction = true;
        }
        if tiles_to_raster.is_empty() {
            return RasteredLayer {
                rects_painted_straight,
                tile_count: 0,
            };
        }

        let pooled_surface_and_rect_it_holds = |tile_pool: &TilePool, tile: &TileShownInFrame| {
            let pooled = tile_pool
                .get(&tile.content_hash)
                .expect("a tile to raster is in the pool");
            let surface = pooled.surface().expect("a tile to raster has a surface");
            (
                surface.painting_surface().clone(),
                with_tile_border(pooled.held_part_of_cell),
            )
        };
        let bounds_held_by = |tile_pool: &TilePool, tiles: &[&TileShownInFrame]| {
            tiles
                .iter()
                .filter_map(|tile| tile_pool.get(&tile.content_hash))
                .fold(IntRect::default(), |bounds, pooled| {
                    bounds.united(with_tile_border(pooled.held_part_of_cell))
                })
        };

        for batch in batch_tiles_to_raster_by_cuts(shown, &tiles_to_raster) {
            let batch_tiles: Vec<&TileShownInFrame> = batch
                .iter()
                .map(|&position_in_tiles_to_raster| {
                    &shown[tiles_to_raster[position_in_tiles_to_raster].shown_tile_index]
                })
                .collect();
            let largest_raster_margin_if_every_one_is_known = batch_tiles.iter().try_fold(0i32, |largest, tile| {
                Some(largest.max(tile.raster_margin_in_device_pixels?))
            });
            let was_rastered = if largest_raster_margin_if_every_one_is_known == Some(0) && batch_tiles.len() == 1 {
                let (surface, rect_surface_holds) = pooled_surface_and_rect_it_holds(&self.pool, batch_tiles[0]);
                let save_count = raster.save(&surface);
                raster.translate(&surface, -rect_surface_holds.x as f32, -rect_surface_holds.y as f32);
                raster.clip_rect(&surface, rect_surface_holds);
                raster.clear(&surface, clear_color);
                replay_tiles(&surface, &batch_tiles);
                raster.restore_to_count(&surface, save_count);
                true
            } else {
                // A backdrop filter reading by an unknown reach needs every shown tile cut alike replayed.
                let tiles_replayed: Vec<&TileShownInFrame> = match largest_raster_margin_if_every_one_is_known {
                    Some(_) => batch_tiles.clone(),
                    None => shown.iter().filter(|tile| tile.cuts == batch_tiles[0].cuts).collect(),
                };
                let raster_margin = largest_raster_margin_if_every_one_is_known.unwrap_or(0);
                let mut scratch_rect_in_space = bounds_held_by(&self.pool, &tiles_replayed).inflated_edges(
                    raster_margin,
                    raster_margin,
                    raster_margin,
                    raster_margin,
                );
                if let Some(rastered_within) = rastered_within {
                    scratch_rect_in_space = scratch_rect_in_space.intersected(rastered_within);
                }
                scratch_rect_in_space = scratch_rect_in_space.united(bounds_held_by(&self.pool, &batch_tiles));
                let target_tiles: Vec<(Rc<PaintingSurface>, IntRect)> = batch_tiles
                    .iter()
                    .map(|tile| pooled_surface_and_rect_it_holds(&self.pool, tile))
                    .collect();
                let scratch = raster_scratch_surface_of_size(
                    &mut self.raster_scratch_surface,
                    env,
                    size_of(scratch_rect_in_space),
                    like,
                );
                if let Some(scratch) = &scratch {
                    replay_into_scratch_and_copy_into_tiles(
                        raster,
                        scratch,
                        &target_tiles,
                        scratch_rect_in_space,
                        clear_color,
                        &|target| {
                            replay_tiles(target, &tiles_replayed);
                        },
                    );
                }
                scratch.is_some()
            };
            for position_in_tiles_to_raster in batch {
                tiles_to_raster[position_in_tiles_to_raster].was_rastered = was_rastered;
            }
        }

        for tile_to_raster in &tiles_to_raster {
            let tile = &shown[tile_to_raster.shown_tile_index];
            let Some(pooled) = self.pool.get_mut(&tile.content_hash) else {
                continue;
            };
            pooled.holds_current_pixels = tile_to_raster.was_rastered;
            if !tile_to_raster.was_rastered {
                rects_painted_straight.push(pooled.held_part_of_cell);
                record(
                    tile.coord,
                    RasterCause::PaintedStraightIntoTheFrame,
                    pooled.held_part_of_cell,
                );
            } else if let Some(cause) = tile_to_raster.raster_cause_for_overlay {
                record(tile.coord, cause, pooled.held_part_of_cell);
            }
        }
        RasteredLayer {
            rects_painted_straight,
            tile_count: tiles_to_raster.len() as u64,
        }
    }
}

fn raster_scratch_surface_of_size(
    raster_scratch_surface: &mut Option<Rc<PaintingSurface>>,
    env: &PresentingEnvironment<'_>,
    size: IntSize,
    like: &Rc<PaintingSurface>,
) -> Option<Rc<PaintingSurface>> {
    let cached_surface_fits = raster_scratch_surface
        .as_ref()
        .is_some_and(|surface| surface.size() == size && surface.shares_backend_with(like));
    if !cached_surface_fits {
        *raster_scratch_surface = None;
        *raster_scratch_surface = Some(create_surface_on_backend_of(env.raster, size, like)?);
    }
    env.host.set_timer(
        env.context_id,
        ContextTimer::TileRasterScratchSurfaceRelease,
        RASTER_SCRATCH_SURFACE_RELEASE_DELAY,
    );
    raster_scratch_surface.clone()
}

fn replay_into_scratch_and_copy_into_tiles(
    raster: &RasterHost,
    scratch: &Rc<PaintingSurface>,
    target_tiles: &[(Rc<PaintingSurface>, IntRect)],
    scratch_rect_in_space: IntRect,
    clear_color: Color,
    replay: &dyn Fn(&Rc<PaintingSurface>),
) {
    let save_count = raster.save(scratch);
    raster.clear(scratch, clear_color);
    raster.translate(
        scratch,
        -scratch_rect_in_space.x as f32,
        -scratch_rect_in_space.y as f32,
    );
    replay(scratch);
    raster.restore_to_count(scratch, save_count);
    for (tile_surface, tile_rect_in_space) in target_tiles {
        raster.draw_surface(
            tile_surface,
            scratch,
            tile_rect_in_space.translated(-scratch_rect_in_space.x, -scratch_rect_in_space.y),
            rect_of_size(size_of(*tile_rect_in_space)),
            SurfaceBlend::Source,
            SurfaceSampling::Nearest,
        );
    }
}

// A surface that fell back to the CPU on a GPU backend would be allocated again with every frame.
fn create_surface_on_backend_of(
    raster: &RasterHost,
    size: IntSize,
    like: &Rc<PaintingSurface>,
) -> Option<Rc<PaintingSurface>> {
    let inherits_lcd_text = true;
    raster
        .create_surface(size, Some(like), inherits_lcd_text)
        .filter(|surface| surface.shares_backend_with(like))
}

/// A surface for a tile, which the window server can show as it is where it shows tiles and such a surface can be
/// made.
fn create_tile_surface_on_backend_of(
    raster: &RasterHost,
    size: IntSize,
    like: &Rc<PaintingSurface>,
    window_server_shows_tiles: bool,
) -> Option<Rc<PresentableSurface>> {
    let surface_window_server_can_show = window_server_shows_tiles
        .then(|| raster.create_presentable_surface(size, like))
        .flatten()
        .filter(|surface| {
            surface.can_be_shown_by_window_server() && surface.painting_surface().shares_backend_with(like)
        });
    surface_window_server_can_show
        .or_else(|| create_surface_on_backend_of(raster, size, like).map(PresentableSurface::shown_by_no_window_server))
}

fn raster_cause_of(
    tile_held_at_cell: Option<(&PooledTile, bool)>,
    part_of_cell_needed: IntRect,
    surface_can_be_drawn_and_shown: impl Fn(&Rc<PresentableSurface>) -> bool,
) -> RasterCause {
    let Some((held_tile, held_content_hash_is_needed)) = tile_held_at_cell else {
        return RasterCause::NewlyShown;
    };
    if !held_tile.held_part_of_cell.contains_rect(part_of_cell_needed) {
        return RasterCause::HeldLessThanNeeded;
    }
    if held_tile
        .surface()
        .is_some_and(|surface| !surface_can_be_drawn_and_shown(surface))
    {
        return RasterCause::HeldOnAnotherBackend;
    }
    if held_content_hash_is_needed {
        RasterCause::HeldPixelsInvalidated
    } else {
        RasterCause::ContentHashChanged
    }
}

pub(super) fn joined_rects_of_grid_given_row_by_row<T: Copy + PartialEq>(
    rects: impl IntoIterator<Item = (IntRect, T)>,
) -> Vec<(IntRect, T)> {
    let mut rects_joined_within_rows: Vec<(IntRect, T)> = Vec::new();
    for (rect, value) in rects {
        if rect.is_empty() {
            continue;
        }
        if let Some((left, left_value)) = rects_joined_within_rows.last_mut()
            && *left_value == value
            && left.y == rect.y
            && left.height == rect.height
            && left.right() == rect.x
        {
            left.width += rect.width;
            continue;
        }
        rects_joined_within_rows.push((rect, value));
    }
    let mut joined: Vec<(IntRect, T)> = Vec::new();
    let mut index_of_joined_rect_by_left_width_and_bottom: HashMap<(i32, i32, i32), usize> = HashMap::new();
    for (row_rect, value) in rects_joined_within_rows {
        let left_width_and_bottom_of_rect_above = (row_rect.x, row_rect.width, row_rect.y);
        match index_of_joined_rect_by_left_width_and_bottom.get(&left_width_and_bottom_of_rect_above) {
            Some(&index) if joined[index].1 == value => {
                index_of_joined_rect_by_left_width_and_bottom.remove(&left_width_and_bottom_of_rect_above);
                joined[index].0.height += row_rect.height;
                index_of_joined_rect_by_left_width_and_bottom
                    .insert((row_rect.x, row_rect.width, joined[index].0.bottom()), index);
            }
            _ => {
                index_of_joined_rect_by_left_width_and_bottom
                    .insert((row_rect.x, row_rect.width, row_rect.bottom()), joined.len());
                joined.push((row_rect, value));
            }
        }
    }
    joined
}

fn held_tile_lies_in_shown_rect_at_unshown_cell(
    coord: TileCoord,
    held_tile: &PooledTile,
    rect_shown_in_layer_space: IntRect,
    shown_sorted_by_cell: &[TileShownInFrame],
) -> bool {
    !held_tile
        .held_part_of_cell
        .intersected(rect_shown_in_layer_space)
        .is_empty()
        && shown_sorted_by_cell
            .binary_search_by(|tile| tile.coord.cmp(&coord))
            .is_err()
}

struct TileToRaster {
    shown_tile_index: usize,
    raster_cause_for_overlay: Option<RasterCause>,
    was_rastered: bool,
}

fn batch_tiles_to_raster_by_cuts(shown: &[TileShownInFrame], tiles_to_raster: &[TileToRaster]) -> Vec<Vec<usize>> {
    let mut batches: Vec<(&BarrierCutsOfCell, Vec<usize>)> = Vec::new();
    for (position_in_tiles_to_raster, tile_to_raster) in tiles_to_raster.iter().enumerate() {
        let cuts = &shown[tile_to_raster.shown_tile_index].cuts;
        match batches.iter_mut().find(|(batch_cuts, _)| *batch_cuts == cuts) {
            Some((_, batch)) => batch.push(position_in_tiles_to_raster),
            None => batches.push((cuts, vec![position_in_tiles_to_raster])),
        }
    }
    batches.into_iter().map(|(_, batch)| batch).collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::display_list::layers::{LayerIndex, PlacementNodeIdentity};
    use libgfx_rust::AffineTransform;

    const VIEWPORT: IntRect = IntRect::new(0, 0, 800, 600);

    const LAYER: LayerIdentity = LayerIdentity::Painted {
        node: PlacementNodeIdentity(1),
        band_ordinal_in_node: 0,
        above_barrier: None,
    };

    fn layer_composited_through(transform: AffineTransform) -> LayerCompositingInFrame {
        LayerCompositingInFrame {
            layer_index: LayerIndex(1),
            identity: LAYER,
            space_to_frame: SpaceToFrame::Transform(transform),
            composite_clip: None,
            rounded_composite_clips: Vec::new(),
            shown_by_window_server: false,
        }
    }

    fn one_color_tiles_at(coords: &[TileCoord]) -> LayerTiles {
        let by_cell: BTreeMap<TileCoord, ContentHash> = coords
            .iter()
            .enumerate()
            .map(|(index, coord)| (*coord, index as ContentHash + 1))
            .collect();
        let pool: TilePool = by_cell
            .iter()
            .map(|(coord, content_hash)| {
                (
                    *content_hash,
                    PooledTile {
                        // A color of its own, so that no two tiles join into one fill.
                        pixels: TilePixels::OneColor(Color::from_rgb(0, 128, *content_hash as u8)),
                        held_part_of_cell: coord.cell_rect(),
                        holds_current_pixels: true,
                        last_held_at: MonotonicTime::default(),
                        held_at_latest_eviction: false,
                    },
                )
            })
            .collect();
        LayerTiles {
            held_by_layer: HashMap::from([(LAYER, HeldTiles { by_cell })]),
            pool,
            ..LayerTiles::default()
        }
    }

    fn rects_of_draws(draws: &[TileDraw<'_>]) -> Vec<IntRect> {
        draws
            .iter()
            .map(|draw| match draw {
                TileDraw::TileSurface {
                    rect_in_layer_space, ..
                }
                | TileDraw::FillOfJoinedOneColorTiles {
                    rect_in_layer_space, ..
                } => *rect_in_layer_space,
            })
            .collect()
    }

    // A layer of another process can show many tiles of one color, with colors that keep any two from joining.
    #[test]
    fn rects_of_a_grid_given_row_by_row_join_into_columns_without_a_search_of_every_joined_rect() {
        let cell = |column: i32, row: i32| IntRect::new(column * 16, row * 16, 16, 16);
        let grid = |columns: i32, rows: i32, value_of_cell: fn(i32, i32) -> i32| {
            (0..rows)
                .flat_map(move |row| (0..columns).map(move |column| (cell(column, row), value_of_cell(column, row))))
        };
        assert_eq!(
            joined_rects_of_grid_given_row_by_row(grid(300, 300, |column, _| column / 100)),
            [
                (IntRect::new(0, 0, 1600, 4800), 0),
                (IntRect::new(1600, 0, 1600, 4800), 1),
                (IntRect::new(3200, 0, 1600, 4800), 2),
            ]
        );
        let checkerboard = joined_rects_of_grid_given_row_by_row(grid(300, 300, |column, row| (column + row) % 2));
        assert_eq!(checkerboard.len(), 300 * 300);
        assert_eq!(checkerboard[300 * 300 - 1], (cell(299, 299), 0));
    }

    // A box that the compositor scales up from nothing shows at a tiny part of its raster scale at first.
    #[test]
    fn a_layer_shown_at_a_tiny_scale_draws_its_held_tiles_without_visiting_every_cell_the_viewport_maps_to() {
        let tiny_scale = 1.0e-5;
        let layer = layer_composited_through(AffineTransform::new(tiny_scale, 0.0, 0.0, tiny_scale, 400.0, 300.0));
        let shown_rect = layer.rect_shown_in_layer_space(VIEWPORT, 0);
        assert!(CellRange::touched_by(shown_rect).cell_count() > 1_000_000_000);
        let held_cells = [TileCoord::new(-1, -1), TileCoord::new(0, 0), TileCoord::new(3, 0)];
        let tiles = one_color_tiles_at(&held_cells);
        let draws = tiles.draws_reaching_into(&layer, VIEWPORT);
        assert_eq!(rects_of_draws(&draws), held_cells.map(TileCoord::cell_rect));
    }

    // A frame lets go of a tile that the screen showed through a spell of no frames.
    #[test]
    fn a_tile_let_go_after_a_spell_of_no_frames_stays_pooled_for_a_while() {
        let mut tiles = one_color_tiles_at(&[TileCoord::new(0, 0)]);
        let at_start = MonotonicTime::from_nanoseconds(1_000_000_000);
        assert_eq!(tiles.evict_spare_tiles_at(at_start), None);
        let after_spell_of_no_frames = at_start + Duration::from_milliseconds(10_000);
        tiles.keep_only_layers(&HashSet::new());
        assert_eq!(
            tiles.evict_spare_tiles_at(after_spell_of_no_frames),
            Some(TIME_SPARE_TILE_STAYS_POOLED_FOR_CONTENT_THAT_COMES_BACK)
        );
        assert_eq!(tiles.pool.len(), 1);
    }

    #[test]
    fn a_layer_whose_transform_is_not_finite_or_has_no_inverse_shows_nothing() {
        let tiles = one_color_tiles_at(&[TileCoord::new(0, 0)]);
        for transform in [
            AffineTransform::new(f32::NAN, 0.0, 0.0, 1.0, 0.0, 0.0),
            AffineTransform::new(1.0, 0.0, 0.0, 1.0, f32::INFINITY, 0.0),
            AffineTransform::new(0.0, 0.0, 0.0, 1.0, 0.0, 0.0),
            AffineTransform::new(1.0e-30, 0.0, 0.0, 1.0e-30, 0.0, 0.0),
        ] {
            let layer = layer_composited_through(transform);
            assert_eq!(layer.rect_shown_in_layer_space(VIEWPORT, 0), IntRect::default());
            assert_eq!(
                layer.rect_in_frame(TileCoord::new(0, 0).cell_rect(), VIEWPORT),
                IntRect::default()
            );
            assert!(tiles.draws_reaching_into(&layer, VIEWPORT).is_empty());
        }
    }
}
