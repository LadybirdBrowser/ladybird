/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a `url()` resolves against: the document's base URL, and the resource context of the style
//! sheet a rule came from, keyed by the native sheet's identity (an imported sheet's own, not its
//! importer's). The host lends them at each style transaction boundary; the engine keeps a copy.

use super::fast_hash::FastMap as HashMap;

use super::bridge::{FfiDocumentStyleComputationInputs, FfiHostHandle, FfiStyleSheetResourceContextEntry};
use crate::css::style_compute::FfiStyleSheetResourceContext;

#[derive(Debug, Default, PartialEq)]
pub(crate) struct StyleSheetResourceContext {
    pub(crate) base_url: Box<[u8]>,
    pub(crate) has_base_url: bool,
    pub(crate) origin_clean: bool,
}

#[derive(Debug, Default)]
pub(crate) struct DocumentResourceContexts {
    pub(crate) document_base_url: Box<[u8]>,
    by_source: HashMap<u64, StyleSheetResourceContext>,
}

impl DocumentResourceContexts {
    /// Take in what the host lent with the inputs, and clear the borrowed fields so the inputs
    /// compare by value from here on. Returns whether a record resolved against the contexts held
    /// before could resolve differently now; a sheet that joins or leaves changes no URL another
    /// sheet's rules resolved. Most transactions lend exactly the contexts already held, which are
    /// then kept as they are.
    ///
    /// # Safety
    /// The borrowed fields must name live buffers of their stated lengths for this call.
    pub(crate) unsafe fn take_in(&mut self, inputs: &mut FfiDocumentStyleComputationInputs) -> bool {
        fn lent<'a>(address: *const u8, length: usize) -> &'a [u8] {
            if address.is_null() || length == 0 {
                &[]
            } else {
                unsafe { std::slice::from_raw_parts(address, length) }
            }
        }
        let document_base_url = lent(
            inputs.document_base_url.as_pointer().cast(),
            inputs.document_base_url_length,
        );
        let entries =
            if inputs.style_sheet_resource_contexts.is_none() || inputs.style_sheet_resource_context_count == 0 {
                &[][..]
            } else {
                unsafe {
                    std::slice::from_raw_parts(
                        inputs
                            .style_sheet_resource_contexts
                            .as_pointer()
                            .cast::<FfiStyleSheetResourceContextEntry>(),
                        inputs.style_sheet_resource_context_count,
                    )
                }
            };
        inputs.document_base_url = FfiHostHandle::default();
        inputs.document_base_url_length = 0;
        inputs.style_sheet_resource_contexts = FfiHostHandle::default();
        inputs.style_sheet_resource_context_count = 0;

        let base_url_moved = *self.document_base_url != *document_base_url;
        if base_url_moved {
            self.document_base_url = document_base_url.into();
        }
        let context_of = |entry: &FfiStyleSheetResourceContextEntry| StyleSheetResourceContext {
            base_url: lent(entry.base_url, entry.base_url_length).into(),
            has_base_url: entry.has_base_url,
            origin_clean: entry.origin_clean,
        };
        let held = |entry: &FfiStyleSheetResourceContextEntry| {
            self.by_source.get(&entry.source_identity).is_some_and(|context| {
                *context.base_url == *lent(entry.base_url, entry.base_url_length)
                    && context.has_base_url == entry.has_base_url
                    && context.origin_clean == entry.origin_clean
            })
        };
        if entries.len() == self.by_source.len() && entries.iter().all(held) {
            return base_url_moved;
        }
        let next: HashMap<_, _> = entries
            .iter()
            .map(|entry| (entry.source_identity, context_of(entry)))
            .collect();
        let moved = self
            .by_source
            .iter()
            .any(|(source, context)| next.get(source).is_some_and(|next| next != context));
        self.by_source = next;
        base_url_moved || moved
    }

    pub(crate) fn for_source(&self, source_identity: u64) -> Option<&StyleSheetResourceContext> {
        self.by_source.get(&source_identity)
    }
}

impl StyleSheetResourceContext {
    /// The context as the drive reads it, borrowing this one's base URL.
    pub(crate) fn as_drive_context(&self) -> FfiStyleSheetResourceContext {
        FfiStyleSheetResourceContext {
            base_url: if self.has_base_url {
                self.base_url.as_ptr()
            } else {
                std::ptr::null()
            },
            base_url_length: if self.has_base_url { self.base_url.len() } else { 0 },
            has_value: true,
            origin_clean: self.origin_clean,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn lend(
        contexts: &mut DocumentResourceContexts,
        document_base_url: &str,
        sheets: &[(u64, &str)],
    ) -> (bool, FfiDocumentStyleComputationInputs) {
        let entries: Vec<_> = sheets
            .iter()
            .map(|&(source_identity, base_url)| FfiStyleSheetResourceContextEntry {
                source_identity,
                base_url: base_url.as_ptr(),
                base_url_length: base_url.len(),
                has_base_url: true,
                origin_clean: true,
            })
            .collect();
        let mut inputs = FfiDocumentStyleComputationInputs {
            document_base_url: FfiHostHandle::from_pointer(document_base_url.as_ptr().cast()),
            document_base_url_length: document_base_url.len(),
            style_sheet_resource_contexts: FfiHostHandle::from_pointer(entries.as_ptr().cast()),
            style_sheet_resource_context_count: entries.len(),
            ..Default::default()
        };
        let moved = unsafe { contexts.take_in(&mut inputs) };
        (moved, inputs)
    }

    #[test]
    fn records_move_only_where_a_held_context_resolves_differently() {
        let mut contexts = DocumentResourceContexts::default();
        let (moved, inputs) = lend(&mut contexts, "https://a/", &[(1, "https://a/one.css")]);
        assert!(moved, "the document's base URL moved from none");
        assert!(inputs.document_base_url.is_none() && inputs.style_sheet_resource_contexts.is_none());
        assert_eq!(inputs.style_sheet_resource_context_count, 0);

        // The same contexts again, from other buffers.
        let url = String::from("https://a/one.css");
        assert!(!lend(&mut contexts, &String::from("https://a/"), &[(1, &url)]).0);

        // A sheet that joins, or leaves, resolves nothing another sheet's rules resolved.
        assert!(
            !lend(
                &mut contexts,
                "https://a/",
                &[(1, "https://a/one.css"), (2, "https://b/")]
            )
            .0
        );
        assert_eq!(
            contexts.for_source(2).map(|context| &*context.base_url),
            Some(&b"https://b/"[..])
        );
        assert!(!lend(&mut contexts, "https://a/", &[(1, "https://a/one.css")]).0);
        assert!(contexts.for_source(2).is_none());

        // A held sheet whose base URL moved does.
        assert!(lend(&mut contexts, "https://a/", &[(1, "https://c/one.css")]).0);
        assert_eq!(
            contexts.for_source(1).map(|context| &*context.base_url),
            Some(&b"https://c/one.css"[..])
        );
        assert!(lend(&mut contexts, "https://d/", &[(1, "https://c/one.css")]).0);
        assert_eq!(&*contexts.document_base_url, b"https://d/");
    }
}
