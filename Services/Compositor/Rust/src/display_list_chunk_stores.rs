/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The chunks of the display lists each WebContent client sent for each of its contexts. A client sends the chunks of
//! a display list that the latest lists of the context do not name, and names the rest: both ends keep the chunks of
//! the latest lists by the same rule, so neither tells the other what it keeps.

use crate::host::WebContentClientId;
use libcompositing_rust::display_list::chunks::{ChunkId, KeptChunks};
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::display_list::storage::ListChunk;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

#[derive(Default)]
struct ContextChunks {
    kept: KeptChunks,
    chunks: HashMap<ChunkId, Arc<ListChunk>>,
}

#[derive(Default)]
pub struct DisplayListChunkStores {
    stores: HashMap<(WebContentClientId, CompositorContextId), ContextChunks>,
    /// Contexts that were destroyed, whose ids never come back: what a client still sends for one goes nowhere.
    destroyed_contexts: HashSet<CompositorContextId>,
}

impl DisplayListChunkStores {
    /// Takes in the display list `client` sent for `context`, which names `chunks_in_paint_order` and carries `sent`:
    /// keeps what it sent and answers the chunks it names in paint order, or none for a context that is gone. A list
    /// that names a chunk it did not send and the context does not keep is a protocol error.
    pub fn take_in(
        &mut self,
        client: WebContentClientId,
        context: CompositorContextId,
        sent: Vec<ListChunk>,
        chunks_in_paint_order: &[ChunkId],
    ) -> Result<Option<Vec<Arc<ListChunk>>>, &'static str> {
        if self.destroyed_contexts.contains(&context) {
            return Ok(None);
        }
        let store = self.stores.entry((client, context)).or_default();
        let sent_ids: Vec<ChunkId> = sent.iter().map(|chunk| chunk.id).collect();
        for chunk in sent {
            // The bytes a name was first received with are the ones it stands for from then on.
            store.chunks.entry(chunk.id).or_insert_with(|| Arc::new(chunk));
        }
        let list = chunks_in_paint_order
            .iter()
            .map(|id| store.chunks.get(id).cloned())
            .collect::<Option<Vec<_>>>()
            .ok_or("WebContent published a display list that names a chunk the compositor does not keep");
        for dropped in store.kept.take_in(chunks_in_paint_order) {
            store.chunks.remove(&dropped);
        }
        for id in sent_ids {
            if !store.kept.keeps(id) {
                store.chunks.remove(&id);
            }
        }
        list.map(Some)
    }

    /// Forgets the chunks every client sent for `context`, which was destroyed.
    pub fn forget_context(&mut self, context: CompositorContextId) {
        self.stores.retain(|(_, store_context), _| *store_context != context);
        self.destroyed_contexts.insert(context);
    }
}

/// The chunks a display list sent whole names in paint order, or an error where it names one it did not send.
pub fn chunks_of_list_sent_whole(
    sent: Vec<ListChunk>,
    chunks_in_paint_order: &[ChunkId],
) -> Result<Vec<Arc<ListChunk>>, &'static str> {
    let sent: HashMap<ChunkId, Arc<ListChunk>> = sent.into_iter().map(|chunk| (chunk.id, Arc::new(chunk))).collect();
    chunks_in_paint_order
        .iter()
        .map(|id| sent.get(id).cloned())
        .collect::<Option<Vec<_>>>()
        .ok_or("WebContent sent a display list that names a chunk it does not carry")
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::display_list::builder::DisplayListBuilder;
    use libcompositing_rust::display_list::chunks::{Chunk, DISPLAY_LISTS_WHOSE_CHUNKS_ARE_KEPT};
    use libcompositing_rust::display_list::commands::{ContextRef, EffectNodeIndex, FillRect};
    use libcompositing_rust::display_list::storage::{
        PackedChunkSize, packed_size_of_chunks, receive_packed_chunks, write_packed_chunks,
    };
    use libgfx_rust::{Color, CompositingAndBlendingOperator, IntRect};

    fn chunk(y: i32) -> Chunk {
        let mut builder = DisplayListBuilder::new();
        builder.append(
            &FillRect {
                rect: IntRect::new(0, y, 10, 10),
                color: Color::default(),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
        );
        builder.take_chunk()
    }

    fn sent(chunks: &[&Chunk]) -> Vec<ListChunk> {
        let mut packed = vec![0; packed_size_of_chunks(chunks)];
        write_packed_chunks(chunks, &mut packed);
        let sizes: Vec<PackedChunkSize> = chunks.iter().map(|chunk| PackedChunkSize::of(chunk)).collect();
        receive_packed_chunks(&packed, &sizes).unwrap()
    }

    #[test]
    fn a_list_names_what_the_latest_lists_of_its_context_kept_and_sends_the_rest() {
        let client = WebContentClientId(1);
        let context = CompositorContextId(7);
        let (first, second, third) = (chunk(0), chunk(10), chunk(20));
        let mut stores = DisplayListChunkStores::default();

        let list = stores
            .take_in(client, context, sent(&[&first, &second]), &[first.id, second.id])
            .unwrap()
            .unwrap();
        assert_eq!(
            list.iter().map(|chunk| chunk.id).collect::<Vec<_>>(),
            [first.id, second.id]
        );

        // The next list sends only what is new to the context.
        let list = stores
            .take_in(client, context, sent(&[&third]), &[third.id, first.id])
            .unwrap()
            .unwrap();
        assert_eq!(
            list.iter().map(|chunk| chunk.id).collect::<Vec<_>>(),
            [third.id, first.id]
        );

        // Another client, or another context, keeps nothing of it.
        assert!(
            stores
                .take_in(WebContentClientId(2), context, Vec::new(), &[first.id])
                .is_err()
        );
        assert!(
            stores
                .take_in(client, CompositorContextId(8), Vec::new(), &[first.id])
                .is_err()
        );

        // A chunk no kept list names is gone.
        for _ in 0..DISPLAY_LISTS_WHOSE_CHUNKS_ARE_KEPT {
            stores.take_in(client, context, Vec::new(), &[third.id]).unwrap();
        }
        assert!(stores.take_in(client, context, Vec::new(), &[second.id]).is_err());
    }

    #[test]
    fn what_a_client_sends_for_a_destroyed_context_goes_nowhere() {
        let client = WebContentClientId(1);
        let context = CompositorContextId(7);
        let first = chunk(0);
        let mut stores = DisplayListChunkStores::default();
        stores.take_in(client, context, sent(&[&first]), &[first.id]).unwrap();
        stores.forget_context(context);
        assert!(
            stores
                .take_in(client, context, Vec::new(), &[first.id])
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn a_list_sent_whole_names_only_what_it_carries() {
        let (first, second) = (chunk(0), chunk(10));
        let list = chunks_of_list_sent_whole(sent(&[&first, &second]), &[second.id, first.id]).unwrap();
        assert_eq!(
            list.iter().map(|chunk| chunk.id).collect::<Vec<_>>(),
            [second.id, first.id]
        );
        assert!(chunks_of_list_sent_whole(sent(&[&first]), &[second.id]).is_err());
    }
}
