/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The lines the dump and the overlay of layers tell of a plan, and the layers the dump names for each record.

use super::{AnimatedNode, PlacedEffect, PlannedLayers, WhyEffectIsComposited, for_each_layer_records_go_into};
use crate::display_list::commands::EffectNodeIndex;
use crate::display_list::layers::{
    CompositedEffect, LayerContent, LayerIndex, NodeNamesFromIndices, VisualContextNodeNamer,
    WindowServerPlacementsOfEffects, WindowServerPresentationOfMedia, media_name, wire,
};
use crate::display_list::reach::Reach;
use crate::display_list::storage::ListPosition;
use libgfx_rust::{CornerRadii, IntRect};
use std::collections::HashMap;

impl PlannedLayers {
    // Each effect the stack composites, with why it is composited and what it reads of what is composited below it, as
    // where it is placed tells.
    pub(super) fn composited_effects_with_why_and_backdrop_read_reach(
        &self,
    ) -> impl Iterator<Item = (&CompositedEffect, WhyEffectIsComposited, Reach<IntRect>)> + '_ {
        let placed_effect_by_effect: HashMap<EffectNodeIndex, &PlacedEffect> = self
            .placement
            .placed_effects
            .iter()
            .map(|placed_effect| (placed_effect.effect, placed_effect))
            .collect();
        self.stack.composited_effects.iter().filter_map(move |composited| {
            let placed_effect = placed_effect_by_effect.get(&composited.effect)?;
            // A placed effect whose layer content decides whether it is composited is composited only for this.
            let why = placed_effect
                .why_composited_regardless_of_layer_content
                .unwrap_or(WhyEffectIsComposited::ItReadsWhatItsOwnLayerDoesNotHold);
            Some((composited, why, placed_effect.backdrop_read_reach))
        })
    }

    /// What the dump says of each effect the compositor composites: the space of its node, what it reads of what is
    /// composited below it, and why it is composited.
    pub fn lines_of_composited_effects<'a>(
        &'a self,
        node_namer: &'a dyn VisualContextNodeNamer,
        placements_of_effects: &'a WindowServerPlacementsOfEffects,
    ) -> impl Iterator<Item = String> + 'a {
        let stack = &self.stack;
        self.composited_effects_with_why_and_backdrop_read_reach()
            .map(move |(composited, why, backdrop_read_reach)| {
                let who_composites_it = match placements_of_effects.of_node(composited.placement_node_index) {
                    Some(Ok(_)) => String::from(", can be composited by the window server"),
                    Some(Err(why_window_server_cannot_composite)) => {
                        format!(", composited by the compositor: {why_window_server_cannot_composite}")
                    }
                    None => String::new(),
                };
                let parent_node_index = stack.nodes[composited.placement_node_index.0]
                    .parent_index
                    .expect("a composited node is inside another");
                let backdrop_read_text = match backdrop_read_reach {
                    Reach::Nothing => String::new(),
                    Reach::Within(rect) => format!(" reads=[{},{} {}x{}]", rect.x, rect.y, rect.width, rect.height),
                    Reach::Anywhere => " reads=anywhere".into(),
                };
                format!(
                    "composited {} space={}{backdrop_read_text}: {}{who_composites_it}",
                    node_namer.name_of_effect_node(composited.effect),
                    stack.name_of_node(parent_node_index, node_namer),
                    why
                )
            })
    }

    /// What the dump says of each backdrop filter or blend mode that finds all it reads in the layer it goes into, and
    /// reads it there rather than the compositor compositing it, in paint order.
    pub fn lines_of_effects_reading_only_own_layer<'a>(
        &'a self,
        node_namer: &'a dyn VisualContextNodeNamer,
    ) -> impl Iterator<Item = String> + 'a {
        let composited_effects = &self.stack.composited_effects;
        self.placement
            .backdrop_readers_in_paint_order
            .iter()
            .filter(|reader| {
                composited_effects
                    .iter()
                    .all(|composited| composited.effect != **reader)
            })
            .map(|effect| {
                format!(
                    "{} is not composited: it reads only its own layer",
                    node_namer.name_of_effect_node(*effect)
                )
            })
    }

    /// What the dump says of each canvas, video or nested context that was not given a layer, and why.
    pub fn lines_of_media_without_layers(&self) -> impl Iterator<Item = String> + '_ {
        self.placement
            .media_without_layers
            .iter()
            .map(|(content, why_no_layer)| format!("no layer for {}: {why_no_layer}", media_name(*content)))
    }

    /// The lines the overlay of layers shows of the stack past its layers, which WebContent sends with the plan: those
    /// of the effects the compositor composites, of the canvases, videos and nested contexts without layers, and of the
    /// effects that read only their own layer, with nodes named by their indices, as many and as long as a plan holds.
    /// A stack of one layer that replays the whole list has none.
    pub fn note_lines_for_layers_overlay(&self) -> Vec<String> {
        if self.stack.replays_whole_list() {
            return Vec::new();
        }
        let placements_of_no_frame = WindowServerPlacementsOfEffects::default();
        wire::note_lines_a_plan_holds(
            self.lines_of_composited_effects(&NodeNamesFromIndices, &placements_of_no_frame)
                .chain(self.lines_of_media_without_layers())
                .chain(self.lines_of_effects_reading_only_own_layer(&NodeNamesFromIndices)),
        )
    }

    /// What the dump says of each node the compositor animates whose content was not given layers of its own, and why.
    pub fn lines_of_animations_without_layers<'a>(
        &'a self,
        node_namer: &'a dyn VisualContextNodeNamer,
    ) -> impl Iterator<Item = String> + 'a {
        self.placement
            .animations_without_layers
            .iter()
            .map(|(animated_node, why_no_layer)| {
                let node_name = match animated_node {
                    AnimatedNode::Transform(spatial) => node_namer.name_of_spatial_node(*spatial),
                    AnimatedNode::Effect(effect) => node_namer.name_of_effect_node(*effect),
                };
                format!("no layer for animated {node_name}: {why_no_layer}")
            })
    }

    /// The lines of the Layers section of the dump, each with the layer it is about, if it is about one. There is one
    /// line per layer, bottom to top: its index, what it holds, the space it is rastered in, what it sits above, the
    /// records `describe_records` names for it, what it is clipped to as it is composited, and whether the window
    /// server that `presentation` is of can show it. A layer below the base layer, or one that can hold nothing, is
    /// left out. Then a line for each effect the compositor composites, each canvas, video or nested context without a
    /// layer, each animation without a layer, and each effect that reads only its own layer.
    pub fn layer_lines(
        &self,
        presentation: &WindowServerPresentationOfMedia,
        node_namer: &dyn VisualContextNodeNamer,
        describe_records: impl Fn(LayerIndex) -> Option<String>,
    ) -> Vec<(Option<LayerIndex>, String)> {
        let stack = &self.stack;
        let records_suffix_of = |layer_index: LayerIndex| {
            describe_records(layer_index).map_or_else(String::new, |records| format!(" records={records}"))
        };
        if let Some(why_list_is_one_layer) = stack.why_list_is_one_layer {
            let only_layer = LayerIndex(0);
            return vec![(
                Some(only_layer),
                format!(
                    "{}{}: {why_list_is_one_layer}",
                    stack.name_of_layer(only_layer, node_namer),
                    records_suffix_of(only_layer)
                ),
            )];
        }
        let mut lines = Vec::new();
        for (index, layer) in stack.layers.iter().enumerate() {
            let layer_index = LayerIndex(index);
            if layer_index < stack.base_layer || (layer_index != stack.base_layer && !layer.may_hold_content) {
                continue;
            }
            let node = &stack.nodes[layer.placement_node_index.0];
            let mut line = format!(
                "{}{}",
                stack.name_of_layer(layer_index, node_namer),
                records_suffix_of(layer_index)
            );
            for clip in &node.composite_clips {
                let clip_rect = clip.rect;
                line.push_str(&format!(
                    " composited within [{},{} {}x{}]{} of {}",
                    clip_rect.x,
                    clip_rect.y,
                    clip_rect.width,
                    clip_rect.height,
                    text_of_corner_radii_if_any(clip.corner_radii),
                    stack.name_of_space(clip.raster_space_index, node_namer)
                ));
            }
            if matches!(layer.content, LayerContent::Media { .. }) {
                match presentation.placements.of_layer(layer_index) {
                    Some(Ok(placement)) if presentation.layer_is_shown_by_window_server(layer_index) => {
                        line.push_str(&format!(" can be shown by the window server: {placement}"));
                    }
                    Some(Ok(_)) if stack.layer_is_inside_composited_effects(layer_index) => {
                        line.push_str(" replayed: inside an effect the compositor composites");
                    }
                    Some(Ok(_)) => line.push_str(" replayed: under a layer the frame holds"),
                    Some(Err(why_window_server_cannot_show)) => {
                        line.push_str(&format!(" replayed: {why_window_server_cannot_show}"));
                    }
                    None => {}
                }
            } else if let Some(why_frame_holds_layer) = presentation.why_frame_holds_painted_layer(stack, layer_index) {
                line.push_str(&format!(" held by the frame: {why_frame_holds_layer}"));
            }
            lines.push((Some(layer_index), line));
        }
        lines.extend(
            self.lines_of_composited_effects(node_namer, &presentation.placements_of_effects)
                .chain(self.lines_of_media_without_layers())
                .chain(self.lines_of_animations_without_layers(node_namer))
                .chain(self.lines_of_effects_reading_only_own_layer(node_namer))
                .map(|line| (None, line)),
        );
        lines
    }

    /// The layers that the record at `position` can be drawn into, in the tiles it reaches.
    pub fn layers_of_record(&self, position: ListPosition) -> Vec<LayerIndex> {
        let stack = &self.stack;
        if stack.replays_whole_list() {
            return vec![LayerIndex(0)];
        }
        for (index, layer) in stack.layers.iter().enumerate() {
            let LayerContent::Media { record_span, .. } = &layer.content else {
                continue;
            };
            if record_span.contains(position) {
                return vec![LayerIndex(index)];
            }
        }
        let Some(stretch) = self
            .placement
            .record_stretches
            .iter()
            .find(|stretch| stretch.record_span.contains(position))
        else {
            return Vec::new();
        };
        let Some(band_index) = stack
            .bands
            .iter()
            .position(|band| band.record_spans.iter().any(|span| span.contains(position)))
        else {
            return Vec::new();
        };
        let band = &stack.bands[band_index];
        let mut layers = Vec::new();
        for_each_layer_records_go_into(
            &band.cuts,
            band.layer_below_all_cuts,
            stretch.record_span.begin,
            stretch.reach,
            |layer, _| layers.push(layer),
        );
        layers.sort_unstable();
        layers.dedup();
        layers
    }
}

fn text_of_corner_radii_if_any(radii: CornerRadii) -> String {
    if !radii.has_any_radius() {
        return String::new();
    }
    let corners = [radii.top_left, radii.top_right, radii.bottom_right, radii.bottom_left];
    let every_corner_is_circular = corners
        .iter()
        .all(|corner| corner.horizontal_radius == corner.vertical_radius);
    let corner_texts: Vec<String> = corners
        .iter()
        .map(|corner| {
            if every_corner_is_circular {
                corner.horizontal_radius.to_string()
            } else {
                format!("{}x{}", corner.horizontal_radius, corner.vertical_radius)
            }
        })
        .collect();
    format!(" radii=({})", corner_texts.join(","))
}
