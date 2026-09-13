/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::commands::{ClipNodeIndex, ContextRef, DisplayListCommandRun, EffectNodeIndex};
use crate::painting::visual_context::{VisualContextTree, clip_lowest_common_ancestor_with_depths};

/// Layer placement belongs to a complete display list, before replay culls any runs.
/// The tree supplies the clip where each effect begins; recorded contexts can widen
/// that boundary when positioned descendants escape a clip. Inactive effects do not
/// participate, so appending unused nodes cannot change a retained list's plan.
pub struct EffectClipPlan {
    output_clips: Vec<Option<ClipNodeIndex>>,
}

impl EffectClipPlan {
    pub fn new(tree: &VisualContextTree, runs: &[DisplayListCommandRun]) -> Option<Self> {
        Self::from_contexts(tree, runs.iter().map(|run| run.context))
    }

    pub(crate) fn from_contexts(
        tree: &VisualContextTree,
        contexts: impl IntoIterator<Item = ContextRef>,
    ) -> Option<Self> {
        let mut output_clips = vec![None; tree.effect_nodes.len()];
        let clip_depths = std::cell::OnceCell::new();
        let common_clip = |a: ClipNodeIndex, b: ClipNodeIndex| {
            if a == b {
                return a;
            }
            if a.is_none() || b.is_none() {
                return ClipNodeIndex::NONE;
            }
            let clip_depths = clip_depths.get_or_init(|| tree.clip_depths());
            clip_lowest_common_ancestor_with_depths(
                |index| tree.clip_nodes[index.0 as usize].parent,
                |index| {
                    if index.is_none() {
                        0
                    } else {
                        clip_depths[index.0 as usize]
                    }
                },
                a,
                b,
            )
        };
        let activate = |mut effect: EffectNodeIndex, clips: &mut [Option<ClipNodeIndex>]| {
            while !effect.is_none() && clips[effect.0 as usize].is_none() {
                let node = &tree.effect_nodes[effect.0 as usize];
                clips[effect.0 as usize] = Some(node.local_clip);
                effect = node.parent;
            }
        };
        if let Some(effect) = tree.root_isolation_effect {
            activate(effect, &mut output_clips);
        }
        for context in contexts {
            if !tree.context_is_valid(context) {
                return None;
            }
            if context.effect.is_none() {
                continue;
            }
            activate(context.effect, &mut output_clips);
            let output = &mut output_clips[context.effect.0 as usize];
            *output = Some(common_clip(output.unwrap(), context.clip));
        }
        // A parent's layer must be outside every participating child's output clip.
        for &index in tree.effect_dependency_order().iter().rev() {
            let parent = tree.effect_nodes[index as usize].parent;
            if !parent.is_none()
                && let Some(clip) = output_clips[index as usize]
            {
                let parent_output = &mut output_clips[parent.0 as usize];
                *parent_output = Some(common_clip(parent_output.unwrap(), clip));
            }
        }
        Some(Self { output_clips })
    }

    pub fn output_clip(&self, effect: EffectNodeIndex) -> ClipNodeIndex {
        self.output_clips[effect.0 as usize].expect("an effect used by replay participates in its plan")
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::visual_context::{
        ClipNodeData, EffectNodeData, TransformData, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX,
    };
    use libgfx_rust::{CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, FloatRect};

    fn tree() -> VisualContextTree {
        VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            sorting_context_root_index: None,
            establishes_sorting_context: false,
            synthetic_plane: false,
        })
    }

    fn clip(tree: &mut VisualContextTree, parent: ClipNodeIndex) -> ClipNodeIndex {
        tree.append_clip(
            ClipNodeData::rect_clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
            parent,
            VISUAL_VIEWPORT_NODE_INDEX,
        )
    }

    fn effect(tree: &mut VisualContextTree, parent: EffectNodeIndex, local_clip: ClipNodeIndex) -> EffectNodeIndex {
        tree.append_effect(
            EffectNodeData::layer_blending_with(CompositingAndBlendingOperator::Normal),
            parent,
            VISUAL_VIEWPORT_NODE_INDEX,
            local_clip,
        )
    }

    fn context(effect: EffectNodeIndex, clip: ClipNodeIndex) -> ContextRef {
        ContextRef {
            effect,
            clip,
            ..ContextRef::default()
        }
    }

    #[test]
    fn descendant_clips_do_not_move_the_layer_inside_its_starting_clip() {
        let mut tree = tree();
        let outer = clip(&mut tree, ClipNodeIndex::NONE);
        let inner = clip(&mut tree, outer);
        let effect = effect(&mut tree, EffectNodeIndex::NONE, outer);
        let plan = EffectClipPlan::from_contexts(&tree, [context(effect, inner)]).unwrap();
        assert_eq!(plan.output_clip(effect), outer);
    }

    #[test]
    fn an_escape_widens_enclosing_effects_and_its_removal_tightens_a_new_plan() {
        let mut tree = tree();
        let outer = clip(&mut tree, ClipNodeIndex::NONE);
        let inner = clip(&mut tree, outer);
        let sibling = clip(&mut tree, outer);
        let parent = effect(&mut tree, EffectNodeIndex::NONE, inner);
        let child = effect(&mut tree, parent, inner);
        let ordinary = context(child, inner);
        let escaping = context(child, sibling);
        let old_plan = EffectClipPlan::from_contexts(&tree, [ordinary, escaping]).unwrap();
        assert_eq!(old_plan.output_clip(child), outer);
        assert_eq!(old_plan.output_clip(parent), outer);
        let new_plan = EffectClipPlan::from_contexts(&tree, [ordinary]).unwrap();
        assert_eq!(new_plan.output_clip(child), inner);
        assert_eq!(new_plan.output_clip(parent), inner);
        assert_eq!(old_plan.output_clip(parent), outer);
    }

    #[test]
    fn inactive_effects_cannot_change_a_retained_lists_plan() {
        let mut tree = tree();
        let inner = clip(&mut tree, ClipNodeIndex::NONE);
        let parent = effect(&mut tree, EffectNodeIndex::NONE, inner);
        let contexts = [context(parent, inner)];
        let before = EffectClipPlan::from_contexts(&tree, contexts).unwrap();
        let unused = effect(&mut tree, parent, ClipNodeIndex::NONE);
        // Local clips may cross along an effect chain; only the replay plan's resolved
        // boundaries need to nest. This is valid serialized visual-context state.
        let restored = VisualContextTree::from_bytes(&tree.to_bytes()).unwrap();
        let after = EffectClipPlan::from_contexts(&restored, contexts).unwrap();
        assert_eq!(before.output_clip(parent), inner);
        assert_eq!(after.output_clip(parent), inner);
        assert!(after.output_clips[unused.0 as usize].is_none());
    }

    #[test]
    fn plan_does_not_depend_on_run_order_or_duplicate_contexts() {
        let mut tree = tree();
        let a = clip(&mut tree, ClipNodeIndex::NONE);
        let b = clip(&mut tree, ClipNodeIndex::NONE);
        let effect = effect(&mut tree, EffectNodeIndex::NONE, a);
        let a = context(effect, a);
        let b = context(effect, b);
        let first = EffectClipPlan::from_contexts(&tree, [a, b, a]).unwrap();
        let second = EffectClipPlan::from_contexts(&tree, [b, a]).unwrap();
        assert_eq!(first.output_clips, second.output_clips);
        assert_eq!(first.output_clip(effect), ClipNodeIndex::NONE);
    }

    #[test]
    fn root_isolation_participates_even_without_drawing_runs() {
        let mut tree = tree();
        let root = effect(&mut tree, EffectNodeIndex::NONE, ClipNodeIndex::NONE);
        tree.root_isolation_effect = Some(root);
        let plan = EffectClipPlan::from_contexts(&tree, []).unwrap();
        assert_eq!(plan.output_clip(root), ClipNodeIndex::NONE);
    }

    #[test]
    fn rejects_runs_with_invalid_visual_context_indices() {
        let tree = tree();
        assert!(EffectClipPlan::from_contexts(&tree, [context(EffectNodeIndex(50), ClipNodeIndex::NONE)]).is_none());
        assert!(EffectClipPlan::from_contexts(&tree, [context(EffectNodeIndex::NONE, ClipNodeIndex(50))]).is_none());
    }

    #[test]
    fn plans_can_be_shared_between_replay_threads() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<EffectClipPlan>();
    }
}
