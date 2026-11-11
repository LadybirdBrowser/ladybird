/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Device-pixel offsets keyed by SpatialNodeIndex: the scroll containers' offsets as produced by the document, plus the sticky nodes' offsets derived from them and the tree (by the document's scroll state refresh in process, by resolve_sticky_offsets() in the compositor).

use super::{AccumulatedVisualContextTree, SpatialNodeIndex};
use crate::visual_context::WorldTransforms;
use libgfx_rust::FloatPoint;
use libipc_rust::wire::{Decode, DecodeError, Decoder, Encode, EncodeError, Encoder};
use std::cell::RefCell;
use std::sync::Arc;

// The world transforms built for one visual context tree under a snapshot's offsets. A copy starts empty, so the table
// never outlives the offsets it was built for. A move takes the table along with the offsets.
#[derive(Default)]
pub struct WorldTransformsCache {
    // The cache retains the tree, so the tree it describes cannot change under it: a write to a shared tree copies it.
    cached: RefCell<Option<(AccumulatedVisualContextTree, Arc<WorldTransforms>)>>,
}

impl Clone for WorldTransformsCache {
    fn clone(&self) -> Self {
        Self::default()
    }
}

impl WorldTransformsCache {
    pub fn get_or_build(
        &self,
        tree: &AccumulatedVisualContextTree,
        device_offsets: &[FloatPoint],
    ) -> Arc<WorldTransforms> {
        let mut cached = self.cached.borrow_mut();
        if let Some((retained_tree, world)) = cached.as_ref()
            && Arc::ptr_eq(retained_tree.as_arc(), tree.as_arc())
        {
            return world.clone();
        }
        *cached = None;
        let world = Arc::new(WorldTransforms::new(tree.as_arc(), device_offsets, None));
        *cached = Some((tree.clone(), world.clone()));
        world
    }

    pub fn clear(&self) {
        *self.cached.borrow_mut() = None;
    }
}

// Stored dense in process so display list replay and hit testing index it directly;
// indices that are not scroll-like nodes read as zero offsets. The IPC representation is sparse
// (index, offset) pairs.
//
// The dense store must never grow past the spatial node count. A compromised WebContent renderer controls the
// indices that reach it — both as decoded wire pairs and as scroll-node indices replayed from the display list. So,
// set_device_offset_for_index() drops any index at or past m_node_count. The compositor supplies that bound from the
// AccumulatedVisualContextTree that owns these nodes. Until it does, the bound is unlimited — which is what the
// trusted in-process snapshot built from live scroll nodes needs.
#[derive(Clone)]
pub struct ScrollStateSnapshot {
    device_offsets: Vec<FloatPoint>,
    staged_offsets: Vec<StagedOffset>,
    node_count: usize,
    world_transforms_cache: WorldTransformsCache,
}

impl Default for ScrollStateSnapshot {
    fn default() -> Self {
        Self {
            device_offsets: Vec::new(),
            staged_offsets: Vec::new(),
            node_count: usize::MAX,
            world_transforms_cache: WorldTransformsCache::default(),
        }
    }
}

impl ScrollStateSnapshot {
    pub fn from_device_offsets(device_offsets: Vec<FloatPoint>) -> Self {
        Self {
            device_offsets,
            ..Self::default()
        }
    }

    // The wire form, for C++ to carry to where Rust decodes it again.
    pub fn to_encoded_bytes(&self) -> Vec<u8> {
        let mut encoder = Encoder::new();
        encoder.encode(self).expect("a scroll state snapshot encodes");
        encoder.finish().0
    }

    pub fn from_encoded_bytes(bytes: &[u8]) -> Result<Self, DecodeError> {
        let mut decoder = Decoder::new(bytes, []);
        let snapshot = decoder.decode()?;
        if !decoder.is_at_end() {
            return Err(DecodeError::InvalidValue(
                "bytes after an encoded scroll state snapshot",
            ));
        }
        Ok(snapshot)
    }

    pub fn device_offsets(&self) -> &[FloatPoint] {
        &self.device_offsets
    }

    // The world transforms of the tree under these offsets, in the layout viewport's space, kept until either changes.
    pub fn world_transforms(&self, tree: &AccumulatedVisualContextTree) -> Arc<WorldTransforms> {
        self.world_transforms_cache.get_or_build(tree, &self.device_offsets)
    }

    pub fn has_only_finite_offsets(&self) -> bool {
        let is_finite = |offset: &FloatPoint| offset.x.is_finite() && offset.y.is_finite();
        self.device_offsets.iter().all(is_finite) && self.staged_offsets.iter().all(|staged| is_finite(&staged.offset))
    }

    pub fn device_offset_for_index(&self, index: SpatialNodeIndex) -> FloatPoint {
        self.device_offsets.get(index.0 as usize).copied().unwrap_or_default()
    }

    pub fn set_device_offset_for_index(&mut self, index: SpatialNodeIndex, offset: FloatPoint) {
        let index = index.0 as usize;
        if index >= self.node_count {
            return;
        }
        self.world_transforms_cache.clear();
        if index >= self.device_offsets.len() {
            self.device_offsets.resize(index + 1, FloatPoint::default());
        }
        self.device_offsets[index] = offset;
    }

    // Replaces every offset with a freshly derived dense array, keeping the node count bound.
    pub fn assign_device_offsets(&mut self, offsets: &[FloatPoint]) {
        self.world_transforms_cache.clear();
        self.device_offsets.clear();
        self.device_offsets
            .extend_from_slice(&offsets[..offsets.len().min(self.node_count)]);
    }

    // Bind the snapshot to the authoritative node count from the AccumulatedVisualContextTree that owns these nodes.
    // Wire-decoded offsets are staged rather than densified. So, this is where a decoded index is first validated:
    // staged pairs are applied through the bounded setter above, dropping any whose index is at or past the node count.
    // And any offset already stored past the count is discarded. Every later mutation stays bounded by the count.
    pub fn set_node_count(&mut self, node_count: usize) {
        self.world_transforms_cache.clear();
        self.node_count = node_count;
        self.device_offsets.truncate(node_count);
        let staged_offsets = std::mem::take(&mut self.staged_offsets);
        for staged in staged_offsets {
            self.set_device_offset_for_index(SpatialNodeIndex(staged.index), staged.offset);
        }
    }
}

// A sparse wire pair captured by the decoder before the node count is known. Held apart from the dense store — so
// that a single decoded pair cannot grow the store to an arbitrary size.
#[derive(Clone)]
struct StagedOffset {
    index: u32,
    offset: FloatPoint,
}

// The dense in-process vector spans the whole spatial node index space, but only scroll and
// sticky node indices can hold non-zero offsets, so the wire format is sparse (index, offset)
// pairs; holes decode back to zero offsets.
impl Encode for ScrollStateSnapshot {
    fn encode(&self, encoder: &mut Encoder) -> Result<(), EncodeError> {
        let device_offsets = self.device_offsets();
        let mut non_zero_offset_count = 0u64;
        for offset in device_offsets {
            if !offset.is_zero() {
                non_zero_offset_count += 1;
            }
        }
        encoder.encode(&non_zero_offset_count)?;
        for (index, offset) in device_offsets.iter().enumerate() {
            if offset.is_zero() {
                continue;
            }
            encoder.encode(&(index as u64))?;
            encoder.encode(offset)?;
        }
        Ok(())
    }
}

impl Decode for ScrollStateSnapshot {
    const MIN_ENCODED_SIZE: usize = size_of::<u64>();

    fn decode(decoder: &mut Decoder<'_>) -> Result<Self, DecodeError> {
        let pair_count = decoder.decode::<u64>()?;
        if pair_count > (decoder.remaining_bytes() / (size_of::<u64>() + FloatPoint::MIN_ENCODED_SIZE)) as u64 {
            return Err(DecodeError::UnexpectedEnd);
        }
        let mut snapshot = Self::default();
        for _ in 0..pair_count {
            let index = decoder.decode::<u64>()?;
            let offset = decoder.decode::<FloatPoint>()?;
            if index >= u32::MAX as u64 {
                return Err(DecodeError::InvalidValue(
                    "IPC decode: ScrollStateSnapshot index out of range",
                ));
            }
            snapshot
                .staged_offsets
                .try_reserve(1)
                .map_err(|_| DecodeError::AllocationFailed)?;
            snapshot.staged_offsets.push(StagedOffset {
                index: index as u32,
                offset,
            });
        }
        Ok(snapshot)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // An index far above any real node count, but still inside the range the decoder accepts. Densifying it would
    // allocate (index + 1) * size_of::<FloatPoint>(), i.e. 512 MiB: the allocation the node-count bound exists to stop.
    const MALICIOUS_INDEX: u32 = 0x0400_0000;

    fn decode_snapshot(pairs: &[(u64, FloatPoint)]) -> ScrollStateSnapshot {
        let mut encoder = Encoder::new();
        encoder.encode(&(pairs.len() as u64)).unwrap();
        for (index, offset) in pairs {
            encoder.encode(index).unwrap();
            encoder.encode(offset).unwrap();
        }
        ScrollStateSnapshot::from_encoded_bytes(&encoder.finish().0).unwrap()
    }

    #[test]
    fn binding_to_node_count_keeps_in_range_and_drops_out_of_range() {
        // A single 16-byte wire pair can claim any node index the decoder accepts. The decoder stages the pair instead
        // of densifying, so decoding cannot abort or over-allocate. The index is validated only once the compositor
        // supplies the node count, which drops it.
        let mut snapshot = decode_snapshot(&[
            (3, FloatPoint { x: 10.0, y: 20.0 }),
            (MALICIOUS_INDEX.into(), FloatPoint { x: 30.0, y: 40.0 }),
        ]);

        snapshot.set_node_count(8);

        assert_eq!(
            snapshot.device_offset_for_index(SpatialNodeIndex(3)),
            FloatPoint { x: 10.0, y: 20.0 }
        );
        assert_eq!(
            snapshot.device_offset_for_index(SpatialNodeIndex(MALICIOUS_INDEX)),
            FloatPoint::default()
        );
        assert!(snapshot.device_offsets().len() <= 8);
    }

    #[test]
    fn set_device_offset_ignores_index_at_or_past_node_count() {
        let mut snapshot = ScrollStateSnapshot::default();
        snapshot.set_node_count(4);

        snapshot.set_device_offset_for_index(SpatialNodeIndex(3), FloatPoint { x: 5.0, y: 6.0 });
        snapshot.set_device_offset_for_index(SpatialNodeIndex(4), FloatPoint { x: 7.0, y: 8.0 });
        snapshot.set_device_offset_for_index(SpatialNodeIndex(MALICIOUS_INDEX), FloatPoint { x: 9.0, y: 9.0 });

        assert_eq!(
            snapshot.device_offset_for_index(SpatialNodeIndex(3)),
            FloatPoint { x: 5.0, y: 6.0 }
        );
        assert_eq!(
            snapshot.device_offset_for_index(SpatialNodeIndex(4)),
            FloatPoint::default()
        );
        assert!(snapshot.device_offsets().len() <= 4);
    }

    #[test]
    fn wire_round_trip_preserves_scroll_offsets() {
        let mut snapshot = ScrollStateSnapshot::default();
        snapshot.set_device_offset_for_index(SpatialNodeIndex(3), FloatPoint { x: 10.0, y: 20.0 });

        let mut decoded = ScrollStateSnapshot::from_encoded_bytes(&snapshot.to_encoded_bytes()).unwrap();
        decoded.set_node_count(8);

        assert_eq!(
            decoded.device_offset_for_index(SpatialNodeIndex(3)),
            FloatPoint { x: 10.0, y: 20.0 }
        );
        // Indices that were never set are holes that decode back to zero offsets.
        assert_eq!(
            decoded.device_offset_for_index(SpatialNodeIndex(1)),
            FloatPoint::default()
        );
    }
}
