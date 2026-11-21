/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the code that a compile job compiled leaves for the functions nested in it, which get compiled later: how the
//! names that they capture resolve (see `resolve.rs`, which works it out for a program).

use crate::ast::Name;
use crate::ast::Resolution;
use crate::ast::SyntaxTree;
use libjs_parser::columns::NONE;
use libjs_parser::resolve::outer_key;
use libjs_parser::token::N_DOT_UNBOUND;
use std::sync::Arc;

/// What a name means from inside a function that a tree did not hold the code of, by function row and name.
#[derive(Clone)]
pub struct OuterResolutions {
    entries: Arc<[(u64, Resolution)]>,
}

impl OuterResolutions {
    /// The resolutions of `entries`, unless there are none (see `new()`).
    pub(crate) fn of_entries(entries: &mut [(u64, Resolution)]) -> Option<Self> {
        (!entries.is_empty()).then(|| Self::new(entries))
    }

    /// The resolutions of `entries`, which get sorted, and of which the first for a name is the one that counts.
    fn new(entries: &mut [(u64, Resolution)]) -> Self {
        if !entries.is_sorted_by_key(|&(key, _)| key) {
            entries.sort_by_key(|&(key, _)| key);
        }
        let mut count = 0;
        for index in 0..entries.len() {
            if count == 0 || entries[count - 1].0 != entries[index].0 {
                entries[count] = entries[index];
                count += 1;
            }
        }
        Self {
            entries: Arc::from(&entries[..count]),
        }
    }

    fn get(&self, row: u32, name: Name) -> Option<Resolution> {
        let key = outer_key(row, name);
        let position = self.entries.binary_search_by_key(&key, |&(key, _)| key).ok()?;
        Some(self.entries[position].1)
    }
}

/// An entry of the resolutions for the function at `row`.
pub(crate) fn outer_entry(row: u32, name: Name, resolution: Resolution) -> (u64, Resolution) {
    (outer_key(row, name), resolution)
}

/// Code that a compile job compiled, as seen by the functions nested in it, which get compiled later.
pub struct EnclosingCode {
    /// The row of the function, or NONE for a program.
    row: u32,
    /// What the functions nested in this code see of it, unless it has none.
    resolutions: Option<OuterResolutions>,
    parent: Option<Arc<EnclosingCode>>,
    /// Whether what this code leaves unbound can go through a direct `eval` of its own.
    has_direct_eval: bool,
}

impl EnclosingCode {
    pub(crate) fn of_program(tree: &SyntaxTree<'_>) -> Arc<Self> {
        Arc::new(Self {
            row: NONE,
            resolutions: tree.outer_resolutions.clone(),
            parent: None,
            has_direct_eval: true,
        })
    }

    /// The code of the function at `row`, from what it found out about what is nested in it itself.
    pub(crate) fn of_function(
        row: u32,
        entries: &mut [(u64, Resolution)],
        parent: &Arc<Self>,
        has_direct_eval: bool,
    ) -> Arc<Self> {
        Arc::new(Self {
            row,
            resolutions: Some(OuterResolutions::new(entries)),
            parent: Some(parent.clone()),
            has_direct_eval,
        })
    }

    /// What `name` means in the function at `row`, nested in this code, if it is free there.
    pub(crate) fn resolution_in_function(
        self: &Arc<Self>,
        script: &super::ScriptData,
        row: u32,
        name: Name,
    ) -> Resolution {
        let mut code = Some(self);
        let mut function = row;
        let mut is_inside_scope_with_eval = false;
        let mut outermost_unbound_name = Resolution::default();
        while let Some(enclosing) = code {
            let resolutions = enclosing
                .resolutions
                .as_ref()
                .expect("code that a function is nested in has resolutions for it");
            if let Some(mut resolution) = resolutions.get(function, name) {
                if is_inside_scope_with_eval {
                    resolution.set_is_inside_scope_with_eval();
                }
                return resolution;
            }
            // NB: The name goes through all the scopes of this code that are around the function. Only the outermost
            //     code has the last word on it, and other code only matters if it has an `eval`.
            if enclosing.has_direct_eval || enclosing.parent.is_none() {
                outermost_unbound_name = resolutions
                    .get(function, Name(N_DOT_UNBOUND))
                    .expect("the code around a function resolved what it does not bind");
                is_inside_scope_with_eval |= outermost_unbound_name.is_inside_tree_scope_with_eval();
            }
            function = enclosing.row;
            code = enclosing.parent.as_ref();
        }
        // NB: No scope around the function binds the name.
        if script.is_global_name_used_in_with(name) {
            outermost_unbound_name.clear_is_global();
        }
        if is_inside_scope_with_eval {
            outermost_unbound_name.set_is_inside_scope_with_eval();
        }
        outermost_unbound_name
    }
}
