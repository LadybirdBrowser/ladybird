/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The identity space of a document's style nodes, which the host owns.
//!
//! The host mints a node's [`StyleNodeID`] where the node connects, without asking the style engine, and tells the
//! engine of the mint ahead of anything it records about the node. The engine gives an identity back only once no
//! reader can still name its previous node: at the finish of the transaction that retired it, which answers with the
//! identities it released. The host mints those again first, latest released first, so the order identities are
//! reused in is the one the engine kept when it owned the space.

use super::tree::StyleNodeID;

/// The identities of one document's style nodes, in two spaces: elements (with shadow roots and the document), and
/// text nodes.
#[derive(Clone)]
pub struct StyleNodeIdAllocator {
    elements: IdentitySpace,
    texts: IdentitySpace,
}

/// One space: the index past every identity minted so far, and the indexes released for reuse.
#[derive(Clone)]
struct IdentitySpace {
    next: u32,
    released: Vec<u32>,
}

impl IdentitySpace {
    /// Index 0 is never an identity.
    const fn new() -> Self {
        Self {
            next: 1,
            released: Vec::new(),
        }
    }

    fn mint(&mut self) -> u32 {
        self.released.pop().unwrap_or_else(|| {
            let index = self.next;
            self.next += 1;
            index
        })
    }
}

impl Default for StyleNodeIdAllocator {
    fn default() -> Self {
        Self {
            elements: IdentitySpace::new(),
            texts: IdentitySpace::new(),
        }
    }
}

impl StyleNodeIdAllocator {
    /// Mints an identity for an element, a shadow root or the document.
    pub fn mint_element(&mut self) -> StyleNodeID {
        StyleNodeID::element(self.elements.mint())
    }

    /// Mints an identity for a text node.
    pub fn mint_text(&mut self) -> StyleNodeID {
        StyleNodeID::text(self.texts.mint())
    }

    /// Takes back the identities a transaction's finish released, in the order it released them.
    pub fn release(&mut self, released: &[u32]) {
        for node in released.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)) {
            match node.text_index() {
                Some(index) => self.texts.released.push(index),
                None => self.elements.released.push(node.raw()),
            }
        }
    }
}

/// Creates the identity space of a new document.
#[unsafe(no_mangle)]
pub extern "C" fn style_node_id_allocator_create() -> *mut StyleNodeIdAllocator {
    Box::into_raw(Box::default())
}

/// # Safety
/// `allocator` must come from [`style_node_id_allocator_create`] and be destroyed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_node_id_allocator_destroy(allocator: *mut StyleNodeIdAllocator) {
    // SAFETY: Guaranteed by the caller.
    drop(unsafe { Box::from_raw(allocator) });
}

/// Mints `count` identities into `out`: text identities where `text`, otherwise element ones.
///
/// # Safety
/// `allocator` must be live, and `out` must point at `count` writable `u32` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_node_id_allocator_mint(
    allocator: *mut StyleNodeIdAllocator,
    text: bool,
    out: *mut u32,
    count: usize,
) {
    if count == 0 {
        return;
    }
    // SAFETY: Guaranteed by the caller.
    let (allocator, out) = unsafe { (&mut *allocator, std::slice::from_raw_parts_mut(out, count)) };
    for slot in out {
        *slot = if text {
            allocator.mint_text()
        } else {
            allocator.mint_element()
        }
        .raw();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identities_are_minted_past_the_last_until_some_are_released() {
        let mut allocator = StyleNodeIdAllocator::default();
        assert_eq!(allocator.mint_element(), StyleNodeID::element(1));
        assert_eq!(allocator.mint_element(), StyleNodeID::element(2));
        assert_eq!(allocator.mint_text(), StyleNodeID::text(1));
        allocator.release(&[StyleNodeID::element(1).raw(), StyleNodeID::text(1).raw()]);
        assert_eq!(allocator.mint_text(), StyleNodeID::text(1));
        assert_eq!(allocator.mint_text(), StyleNodeID::text(2));
        assert_eq!(allocator.mint_element(), StyleNodeID::element(1));
        assert_eq!(allocator.mint_element(), StyleNodeID::element(3));
    }

    #[test]
    fn the_latest_released_identity_is_minted_first() {
        let mut allocator = StyleNodeIdAllocator::default();
        let minted: Vec<_> = (0..3).map(|_| allocator.mint_element().raw()).collect();
        allocator.release(&minted);
        assert_eq!(allocator.mint_element().raw(), minted[2]);
        assert_eq!(allocator.mint_element().raw(), minted[1]);
    }
}
