/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Resolves the identifiers of a parsed tree and numbers its locals, in one walk over the tree.
//!
//! Everything scope analysis decided while parsing is in the tree's scope rows. What is left to do here works scope by
//! scope, innermost first: every scope collects the references in it in groups, one per name, in the order the names
//! came up. When a scope closes, each group either resolves there or moves on to the scope around it, behind the groups
//! of that scope. The results go into the resolution column and the scope table of the tree.
//!
//! A tree only holds the code of one compile job, so this only walks that code. A function nested in it is not in the
//! tree, but what its names resolve to depends on the scopes around it, and those are here now and gone by the time it
//! gets compiled. So every such function leaves a reference behind for each name that it captures, which resolves like
//! any other, and the results stay around as `OuterResolutions` (see `enclosing.rs` in LibJS) for when it does get
//! compiled. A name it uses that the code around it binds further out is a capture of a function further out.
//! `EnclosingCode` links them up.

use super::base::SCOPE_ROW_EVAL_IN_FUNCTION;
use super::base::SCOPE_ROW_POISONED;
use super::base::USE_IN_WITH;
use super::base::decl;
use super::columns::FunctionIndexView;
use super::columns::NONE;
use super::columns::function_flag;
use super::columns::tag;
use super::token::N_ARGUMENTS;
use super::token::N_DOT_NEW_TARGET;
use super::token::N_DOT_SUPER;
use super::token::N_DOT_THIS;
use super::token::N_DOT_UNBOUND;
use super::tree::*;
use crate::u32_from_usize;
use ak::Zone;
use ak::ZoneVec;

#[derive(Clone, Copy, PartialEq, Eq)]
enum FrameKind {
    Program,
    Function,
    Block,
    ForLoop,
    With,
    Catch,
    Class,
    ClassField,
    ClassStaticInitializer,
}

/// A reference to a name, in the chain of the references of its group.
#[derive(Clone, Copy, PartialEq, Eq)]
struct LinkId(u32);

impl LinkId {
    const NONE: Self = Self(NONE);
}

#[derive(Clone, Copy)]
struct NodeLink {
    /// The node that holds the resolution of the reference.
    node: u32,
    next: LinkId,
}

/// What a scope knows of a name, in the [`ScopedNames`] of the resolver.
#[derive(Clone, Copy, PartialEq, Eq)]
struct ScopedNameId(u32);

impl ScopedNameId {
    const NONE: Self = Self(NONE);
}

/// All references to one name in a scope, as the nodes that hold their resolution.
#[derive(Clone, Copy)]
struct Group {
    name: Name,
    /// What the scope that holds the group knows of the name.
    scoped_name: ScopedNameId,
    first_node: LinkId,
    last_node: LinkId,
    declaration_kind: Option<DeclarationKind>,
    used_inside_with_statement: bool,
}

impl Group {
    fn new(name: Name, scoped_name: ScopedNameId) -> Self {
        Self {
            name,
            scoped_name,
            first_node: LinkId::NONE,
            last_node: LinkId::NONE,
            declaration_kind: None,
            used_inside_with_statement: false,
        }
    }

    fn add_node(&mut self, links: &mut ZoneVec<'_, NodeLink>, node: u32) {
        let link = LinkId(u32_from_usize(links.len()));
        links.push(NodeLink {
            node,
            next: LinkId::NONE,
        });
        if self.last_node == LinkId::NONE {
            self.first_node = link;
        } else {
            links[self.last_node.0 as usize].next = link;
        }
        self.last_node = link;
    }

    /// Moves the references of `other` behind the ones of this group.
    fn append_nodes_of(&mut self, links: &mut [NodeLink], other: &Group) {
        if other.first_node == LinkId::NONE {
            return;
        }
        if self.last_node == LinkId::NONE {
            self.first_node = other.first_node;
        } else {
            links[self.last_node.0 as usize].next = other.first_node;
        }
        self.last_node = other.last_node;
    }

    fn nodes<'a>(&self, links: &'a [NodeLink]) -> GroupNodes<'a> {
        GroupNodes {
            links,
            next: self.first_node,
        }
    }
}

struct GroupNodes<'a> {
    links: &'a [NodeLink],
    next: LinkId,
}

impl Iterator for GroupNodes<'_> {
    type Item = u32;

    fn next(&mut self) -> Option<u32> {
        if self.next == LinkId::NONE {
            return None;
        }
        let link = self.links[self.next.0 as usize];
        self.next = link.next;
        Some(link.node)
    }
}

/// Everything that one scope knows of one name.
#[derive(Clone, Copy)]
struct ScopedName {
    name: Name,
    /// The depth of the scope.
    depth: u32,
    /// The same name in the next scope out that knows it.
    outer: ScopedNameId,
    /// The next name that the scope knows.
    next_in_scope: ScopedNameId,
    /// The flags of the name's declarations in the scope.
    binding_flags: u32,
    /// The index of the name's group in the scope, or NONE.
    group: u32,
    /// The position of the parameter of this name, where the last one of a name counts, or NONE.
    parameter_position: u32,
    /// The position among the statements of a function of the last function declaration of this name, or NONE.
    last_function_position: u32,
    local: Option<LocalBinding>,
    /// Whether a declaration of the scope binds the name.
    is_bound: bool,
    /// Whether the name has its place in the scope's order of variables.
    is_in_variable_order: bool,
    is_in_annexb_function_names: bool,
    is_bound_by_parameter_list: bool,
}

/// What the open scopes know of each name. A scope that knows a name has an entry for it, and the entries of a name form
/// a chain from the innermost scope outward, which starts at `innermost[name]`. Names are small numbers, so finding what
/// the innermost scopes know of a name takes no hashing.
struct ScopedNames<'z> {
    innermost: ZoneVec<'z, ScopedNameId>,
    entries: ZoneVec<'z, ScopedName>,
}

impl ScopedNames<'_> {
    fn get(&self, id: ScopedNameId) -> &ScopedName {
        &self.entries[id.0 as usize]
    }

    fn get_mut(&mut self, id: ScopedNameId) -> &mut ScopedName {
        &mut self.entries[id.0 as usize]
    }

    /// The entry of `name` in the scope at `depth`, if that scope knows the name.
    fn find(&self, name: Name, depth: u32) -> Option<&ScopedName> {
        let mut id = *self.innermost.get(name.0 as usize)?;
        while id != ScopedNameId::NONE {
            let entry = self.get(id);
            if entry.depth <= depth {
                return (entry.depth == depth).then_some(entry);
            }
            id = entry.outer;
        }
        None
    }

    /// The flags of the declarations of `name` in the innermost scope that binds it.
    fn innermost_binding(&self, name: Name) -> u32 {
        let mut id = self
            .innermost
            .get(name.0 as usize)
            .copied()
            .unwrap_or(ScopedNameId::NONE);
        while id != ScopedNameId::NONE {
            let entry = self.get(id);
            if entry.is_bound {
                return entry.binding_flags;
            }
            id = entry.outer;
        }
        0
    }

    /// The flags of the declarations of `name` in the scope at `depth`.
    fn binding(&self, name: Name, depth: u32) -> u32 {
        self.find(name, depth).map_or(0, |entry| entry.binding_flags)
    }

    /// The entry of `name` in `frame`, which is new if the scope did not know the name yet.
    fn find_or_add(&mut self, name: Name, frame: &mut Frame<'_>) -> ScopedNameId {
        let index = name.0 as usize;
        if index >= self.innermost.len() {
            self.innermost.resize(index + 1, ScopedNameId::NONE);
        }
        let mut inner = ScopedNameId::NONE;
        let mut id = self.innermost[index];
        while id != ScopedNameId::NONE {
            let entry = self.get(id);
            if entry.depth == frame.depth {
                return id;
            }
            if entry.depth < frame.depth {
                break;
            }
            inner = id;
            id = entry.outer;
        }
        let new_id = ScopedNameId(u32_from_usize(self.entries.len()));
        self.entries.push(ScopedName {
            name,
            depth: frame.depth,
            outer: id,
            next_in_scope: frame.first_name,
            binding_flags: 0,
            group: NONE,
            parameter_position: NONE,
            last_function_position: NONE,
            local: None,
            is_bound: false,
            is_in_variable_order: false,
            is_in_annexb_function_names: false,
            is_bound_by_parameter_list: false,
        });
        frame.first_name = new_id;
        if inner == ScopedNameId::NONE {
            self.innermost[index] = new_id;
        } else {
            self.get_mut(inner).outer = new_id;
        }
        new_id
    }

    /// Forgets what `frame`, the innermost scope, knows.
    fn remove_scope(&mut self, frame: &Frame<'_>) {
        let mut id = frame.first_name;
        while id != ScopedNameId::NONE {
            let entry = *self.get(id);
            self.innermost[entry.name.0 as usize] = entry.outer;
            id = entry.next_in_scope;
        }
    }

    fn touch_variable(&mut self, name: Name, frame: &mut Frame<'_>) {
        let id = self.find_or_add(name, frame);
        let entry = self.get_mut(id);
        if !entry.is_in_variable_order {
            entry.is_in_variable_order = true;
            frame.variable_order.push(name);
        }
    }

    /// The group of `name` in `frame`.
    fn group_for<'f>(&mut self, name: Name, frame: &'f mut Frame<'_>) -> &'f mut Group {
        let id = self.find_or_add(name, frame);
        let entry = self.get_mut(id);
        if entry.group == NONE {
            entry.group = u32_from_usize(frame.groups.len());
            frame.groups.push(Group::new(name, id));
        }
        &mut frame.groups[entry.group as usize]
    }

    /// Whether a function declaration hoisted through the scope at `depth` stops there.
    fn blocks_hoisted_function(&self, name: Name, depth: u32) -> bool {
        let flags = self.binding(name, depth);
        (flags & decl::LEXICAL != 0 && flags & decl::SIMPLE_CATCH == 0) || flags & decl::CATCH_PATTERN != 0
    }
}

/// One scope on the way down the tree.
struct Frame<'z> {
    kind: FrameKind,
    /// How many scopes are around this one.
    depth: u32,
    is_arrow: bool,
    /// The first of the names that this scope knows, in [`ScopedNames`].
    first_name: ScopedNameId,
    row_flags: u32,
    function_flags: u32,
    rest_name: Option<Name>,
    groups: ZoneVec<'z, Group>,
    /// What nested scopes passed on, which goes behind the scope's own groups.
    groups_from_nested_scopes: ZoneVec<'z, Group>,
    functions_to_hoist: ZoneVec<'z, Name>,
    /// What nested scopes passed on, which goes behind the scope's own functions.
    functions_hoisted_from_nested_scopes: ZoneVec<'z, Name>,
    /// The names of the scope in the order the code declares them, with repeats left out.
    variable_order: ZoneVec<'z, Name>,
    local_variables: ZoneVec<'z, LocalVariable>,
    annexb_function_names: ZoneVec<'z, Name>,
    /// Where the block of this scope starts and ends, if the scope is a block.
    block_offsets: Option<(u32, u32)>,
    /// How many loops this scope is in the body of, counting those around the function it is in.
    loop_depth: u32,
    /// The row of the code of a class that runs as a function in this scope.
    code_of_class_row: Option<u32>,
    /// Whether the code that gets compiled as the unit of this scope can change the running environment.
    creates_environment: bool,
}

impl<'z> Frame<'z> {
    fn new(zone: &'z Zone) -> Self {
        Self {
            kind: FrameKind::Block,
            depth: 0,
            is_arrow: false,
            first_name: ScopedNameId::NONE,
            row_flags: 0,
            function_flags: 0,
            rest_name: None,
            groups: ZoneVec::new_in(zone),
            groups_from_nested_scopes: ZoneVec::new_in(zone),
            functions_to_hoist: ZoneVec::new_in(zone),
            functions_hoisted_from_nested_scopes: ZoneVec::new_in(zone),
            variable_order: ZoneVec::new_in(zone),
            local_variables: ZoneVec::new_in(zone),
            annexb_function_names: ZoneVec::new_in(zone),
            block_offsets: None,
            loop_depth: 0,
            code_of_class_row: None,
            creates_environment: false,
        }
    }

    /// Readies a frame that a closed scope left for the scope of `kind` at `depth`.
    fn reset(&mut self, kind: FrameKind, depth: u32) {
        self.kind = kind;
        self.depth = depth;
        self.is_arrow = false;
        self.first_name = ScopedNameId::NONE;
        self.row_flags = 0;
        self.function_flags = 0;
        self.rest_name = None;
        self.groups.clear();
        self.groups_from_nested_scopes.clear();
        self.functions_to_hoist.clear();
        self.functions_hoisted_from_nested_scopes.clear();
        self.variable_order.clear();
        self.local_variables.clear();
        self.annexb_function_names.clear();
        self.block_offsets = None;
        self.code_of_class_row = None;
        self.creates_environment = false;
    }

    fn is_top_level(&self) -> bool {
        matches!(
            self.kind,
            FrameKind::Program | FrameKind::Function | FrameKind::ClassStaticInitializer
        )
    }

    /// Whether the code of this scope gets compiled by itself, without the code around it.
    fn is_compile_unit(&self) -> bool {
        self.is_top_level() || self.kind == FrameKind::ClassField
    }

    /// Whether the bindings that this scope declares itself live in an environment of its own, if they are not locals.
    fn has_environment_for_non_locals(&self) -> bool {
        matches!(self.kind, FrameKind::Block | FrameKind::ForLoop | FrameKind::Catch)
    }

    fn holds_locals(&self) -> bool {
        matches!(self.kind, FrameKind::Function | FrameKind::ClassStaticInitializer)
    }
}

/// A name that a pattern binds, with the node that holds its resolution.
#[derive(Clone, Copy)]
struct BoundName {
    node: u32,
    name: Name,
}

/// What the resolver reads of what is kept of the source of a tree.
#[derive(Clone, Copy)]
pub struct ScriptView<'t> {
    pub index: FunctionIndexView<'t>,
    /// The name of an anonymous default export.
    pub default_export_name: Name,
    pub is_module: bool,
    /// The undeclared names that something in a `with` uses, which are not simply globals, sorted.
    pub global_names_used_in_with: &'t [u32],
}

impl ScriptView<'_> {
    #[inline]
    pub fn is_global_name_used_in_with(&self, name: Name) -> bool {
        self.global_names_used_in_with.binary_search(&name.0).is_ok()
    }
}

/// How identifiers resolve, which the code around the program decides.
pub struct ResolveOptions {
    pub initiated_by_eval: bool,
    pub suppress_globals: bool,
}

/// The scope table that a resolver fills in.
struct ScopeColumns<'z> {
    rows: ZoneVec<'z, ScopeRow>,
    local_variables: ZoneVec<'z, LocalVariable>,
    annexb_function_names: ZoneVec<'z, Name>,
}

/// What resolving found out about a tree, in the work zone, for the tree to refer to.
pub struct Resolved<'z> {
    pub resolutions: &'z [Resolution],
    pub scopes: ScopeTable<'z>,
    pub program_scope_row: u32,
    pub function_scope_rows: &'z [(u32, u32)],
    /// The resolutions for the functions nested in the tree, as (key, resolution), with keys from `outer_key()`. Every
    /// function nested in the code leaves a reference behind, so that without any there are none.
    pub outer_entries: &'z mut [(u64, Resolution)],
}

/// The key of the resolution of `name` for the function at `row`.
pub fn outer_key(row: u32, name: Name) -> u64 {
    u64::from(row) << 32 | u64::from(name.0)
}

struct Resolver<'t, 'z> {
    tree: &'t TreeView<'t>,
    script: ScriptView<'t>,
    zone: &'z Zone,
    resolutions: ZoneVec<'z, Resolution>,
    scopes: ScopeColumns<'z>,
    function_scope_rows: ZoneVec<'z, (u32, u32)>,
    names: ScopedNames<'z>,
    frames: ZoneVec<'z, Frame<'z>>,
    spare_frames: ZoneVec<'z, Frame<'z>>,
    links: ZoneVec<'z, NodeLink>,
    /// The memory for the next list of names that a pattern or parameter list binds.
    bound_names: ZoneVec<'z, BoundName>,
    /// The references that stand for names of functions that get compiled later, as (row, name, node).
    outer_references: ZoneVec<'z, (u32, Name, u32)>,
    /// The links of the chains of operators being walked, see [`Resolver::expression()`].
    chain_links: ZoneVec<'z, u32>,
    /// How many loops the code being walked is in the body of.
    loop_depth: u32,
    is_module: bool,
    initiated_by_eval: bool,
    suppress_globals: bool,
}

impl<'t, 'z> Resolver<'t, 'z> {
    fn new(zone: &'z Zone, tree: &'t TreeView<'t>, script: ScriptView<'t>) -> Self {
        // NB: Each function that the tree does not hold leaves a reference behind for each name that it captures and
        //     one more, see `note_free_names_of_function()`. This counts those of every function in the script, which
        //     is enough for nearly all trees.
        let index = &script.index;
        let outer_reference_count = index.capture_names.len() + index.start.len();
        let mut resolutions = ZoneVec::with_capacity_in(tree.ast.tag.len() + outer_reference_count, zone);
        resolutions.resize(tree.ast.tag.len(), Resolution::default());
        Self {
            zone,
            resolutions,
            scopes: ScopeColumns {
                rows: ZoneVec::new_in(zone),
                local_variables: ZoneVec::new_in(zone),
                annexb_function_names: ZoneVec::new_in(zone),
            },
            function_scope_rows: ZoneVec::new_in(zone),
            names: ScopedNames {
                innermost: ZoneVec::new_in(zone),
                entries: ZoneVec::new_in(zone),
            },
            frames: ZoneVec::new_in(zone),
            spare_frames: ZoneVec::new_in(zone),
            links: ZoneVec::new_in(zone),
            bound_names: ZoneVec::new_in(zone),
            outer_references: ZoneVec::with_capacity_in(outer_reference_count, zone),
            chain_links: ZoneVec::new_in(zone),
            loop_depth: 0,
            is_module: script.is_module,
            initiated_by_eval: false,
            suppress_globals: false,
            tree,
            script,
        }
    }

    /// What the resolver found out.
    fn finish(self) -> Resolved<'z> {
        assert!(self.frames.is_empty());
        let function_scope_rows = self.function_scope_rows.into_slice();
        function_scope_rows.sort_unstable_by_key(|&(node, _)| node);
        let resolutions = self.resolutions.into_slice();
        let outer_entries = self.zone.alloc_slice_from_iter(
            self.outer_references
                .iter()
                .map(|&(row, name, node)| (outer_key(row, name), resolutions[node as usize])),
        );
        let scopes = self.scopes;
        Resolved {
            program_scope_row: u32_from_usize(scopes.rows.len()) - 1,
            resolutions,
            scopes: ScopeTable {
                rows: scopes.rows.into_slice(),
                local_variables: scopes.local_variables.into_slice(),
                annexb_function_names: scopes.annexb_function_names.into_slice(),
            },
            function_scope_rows,
            outer_entries,
        }
    }
}

/// Resolves the identifiers of the tree of a program that the parser just finished, with the work data and the results
/// in `zone`.
pub fn resolve_program<'z>(
    zone: &'z Zone,
    tree: &TreeView<'_>,
    script: &ScriptView<'_>,
    options: &ResolveOptions,
) -> Resolved<'z> {
    let mut resolver = Resolver::new(zone, tree, *script);
    resolver.initiated_by_eval = options.initiated_by_eval;
    resolver.suppress_globals = options.suppress_globals;
    resolver.program(tree.root);
    resolver.finish()
}

impl<'t, 'z> Resolver<'t, 'z> {
    // === Tree access ===

    fn tag(&self, node: u32) -> u8 {
        self.tree.ast.tag[node as usize]
    }

    fn a(&self, node: u32) -> u32 {
        self.tree.ast.a[node as usize]
    }

    fn b(&self, node: u32) -> u32 {
        self.tree.ast.b[node as usize]
    }

    fn pos(&self, node: u32) -> u32 {
        self.tree.ast.pos[node as usize]
    }

    fn extra(&self, index: u32) -> u32 {
        self.tree.ast.extra[index as usize]
    }

    fn list(&self, start: u32) -> &'t [u32] {
        let start = start as usize;
        let count = self.tree.ast.extra[start] as usize;
        &self.tree.ast.extra[start + 1..start + 1 + count]
    }

    /// Where the code after a block starts, given the list of the block's statements.
    fn block_end(&self, list: u32) -> u32 {
        self.extra(list + 1 + self.extra(list))
    }

    fn resolution(&mut self, node: u32) -> &mut Resolution {
        &mut self.resolutions[node as usize]
    }

    // === Scopes ===

    fn merge_row(&mut self, frame: &mut Frame<'z>, row: u32) {
        if row == NONE {
            return;
        }
        let ast = &self.tree.ast;
        let names = &mut self.names;
        let first = ast.scope_first_binding[row as usize] as usize;
        let end = ast
            .scope_first_binding
            .get(row as usize + 1)
            .map_or(ast.binding_name.len(), |&next| next as usize);
        frame.row_flags |= ast.scope_flags[row as usize] & (SCOPE_ROW_EVAL_IN_FUNCTION | SCOPE_ROW_POISONED);
        for binding in first..end {
            let id = names.find_or_add(Name(ast.binding_name[binding]), frame);
            let entry = names.get_mut(id);
            entry.binding_flags |= ast.binding_flags[binding];
            entry.is_bound = true;
        }
    }

    /// A frame for a new scope of `kind` inside the open ones, which is yet to be pushed.
    fn new_frame(&mut self, kind: FrameKind) -> Frame<'z> {
        let zone = self.zone;
        let mut frame = self.spare_frames.pop().unwrap_or_else(|| Frame::new(zone));
        frame.reset(kind, u32_from_usize(self.frames.len()));
        frame.loop_depth = self.loop_depth;
        frame
    }

    fn push_frame(&mut self, kind: FrameKind, row: u32) {
        let mut frame = self.new_frame(kind);
        self.merge_row(&mut frame, row);
        self.frames.push(frame);
    }

    fn top(&mut self) -> &mut Frame<'z> {
        self.frames.last_mut().expect("there is an open scope")
    }

    /// Notes that the code being walked can change the running environment. This is everything that generating code
    /// for it makes an environment for, apart from the declaration instantiation of a function, which `compile.rs` knows
    /// about. Code that turns out to be unreachable counts as well, which only costs saving an environment for nothing.
    fn note_creates_environment(&mut self) {
        if let Some(frame) = self.frames.iter_mut().rev().find(|frame| frame.is_compile_unit()) {
            frame.creates_environment = true;
        }
    }

    /// An empty list for the names that a pattern or parameter list binds, for [`Self::give_back_bound_names()`] after.
    /// Patterns nest, in which case the inner one has to allocate.
    fn take_bound_names(&mut self) -> ZoneVec<'z, BoundName> {
        self.bound_names.take()
    }

    fn give_back_bound_names(&mut self, mut bound: ZoneVec<'z, BoundName>) {
        bound.clear();
        self.bound_names = bound;
    }

    /// Adds a reference to `name` to the innermost scope, whose resolution goes to `node`.
    fn register(&mut self, node: u32, name: Name, declaration_kind: Option<DeclarationKind>) {
        let frame = self.frames.last_mut().expect("there is an open scope");
        let group = self.names.group_for(name, frame);
        group.add_node(&mut self.links, node);
        if declaration_kind.is_some() && group.declaration_kind.is_none() {
            group.declaration_kind = declaration_kind;
        }
    }

    /// Notes that a name is declared with `var` in the scope that the declaration hoists to.
    fn touch_var(&mut self, name: Name) {
        if let Some(frame) = self.frames.iter_mut().rev().find(|frame| frame.is_top_level()) {
            self.names.touch_variable(name, frame);
        }
    }

    /// Closes the innermost scope: resolves what can be resolved in it and passes the rest on to the scope around it.
    /// A scope that holds locals or is the program gets a row in the scope table, which `children` are the statements
    /// of.
    fn close_frame(&mut self, children: &[u32]) -> ScopeRow {
        let mut frame = self.frames.pop().expect("there is an open scope");

        let mut nested_groups = frame.groups_from_nested_scopes.take();
        for nested in &nested_groups {
            let id = self.names.find_or_add(nested.name, &mut frame);
            let entry = self.names.get_mut(id);
            if entry.group == NONE {
                entry.group = u32_from_usize(frame.groups.len());
                frame.groups.push(Group {
                    scoped_name: id,
                    ..*nested
                });
            } else {
                let existing = &mut frame.groups[entry.group as usize];
                existing.append_nodes_of(&mut self.links, nested);
                existing.used_inside_with_statement |= nested.used_inside_with_statement;
            }
        }
        nested_groups.clear();
        frame.groups_from_nested_scopes = nested_groups;

        let mut groups = frame.groups.take();
        let mut passed_on_count = 0;
        for index in 0..groups.len() {
            let mut group = groups[index];
            if let Some(kind) = group.declaration_kind {
                for node in group.nodes(&self.links) {
                    self.resolutions[node as usize].set_declaration_kind(kind);
                }
            }
            let is_passed_on = self.resolve_group(&mut frame, &mut group);
            if frame.has_environment_for_non_locals() {
                let entry = self.names.get(group.scoped_name);
                if entry.binding_flags & (decl::LEXICAL | decl::SIMPLE_CATCH | decl::CATCH_PATTERN) != 0
                    && entry.local.is_none()
                {
                    self.note_creates_environment();
                }
            }
            if is_passed_on {
                groups[passed_on_count] = group;
                passed_on_count += 1;
            }
        }
        groups.truncate(passed_on_count);
        if let Some(row) = frame.code_of_class_row {
            self.note_free_names_of_class_code(row, &mut groups);
        }
        // NB: What leaves the program is unbound, and nothing resolves it further.
        if let Some(parent) = self.frames.last_mut() {
            parent.groups_from_nested_scopes.extend_from_slice(&groups);
        }
        groups.clear();
        frame.groups = groups;

        self.hoist_functions(&mut frame);

        let mut row = ScopeRow {
            creates_environment: frame.creates_environment,
            ..ScopeRow::default()
        };
        if frame.holds_locals() || frame.kind == FrameKind::Program {
            let scopes = &mut self.scopes;
            row.local_variables = append_to_column(&mut scopes.local_variables, &frame.local_variables);
            row.annexb_function_names =
                append_to_column(&mut scopes.annexb_function_names, &frame.annexb_function_names);
        }
        if frame.holds_locals() {
            self.build_function_scope_data(&mut frame, children, &mut row);
        }
        self.names.remove_scope(&frame);
        self.spare_frames.push(frame);
        row
    }

    /// A new node that nothing in the tree refers to, which holds a resolution for a function compiled later.
    fn add_outer_reference(&mut self, row: u32, name: Name) -> u32 {
        let node = u32_from_usize(self.resolutions.len());
        self.resolutions.push(Resolution::default());
        self.outer_references.push((row, name, node));
        node
    }

    /// What a function that is compiled later can see of the code around it comes down to the same references that
    /// the function itself would have left in that code, had it been in the tree.
    fn note_free_names_of_function(&mut self, row: u32) {
        let index = &self.script.index;
        let captures = index.capture_start[row as usize] as usize;
        let count = index.capture_count[row as usize] as usize;
        for &capture in &index.capture_names[captures..captures + count] {
            let name = capture & !USE_IN_WITH;
            if matches!(name, N_DOT_THIS | N_DOT_NEW_TARGET | N_DOT_SUPER) {
                continue;
            }
            self.add_capture(row, Name(name), capture & USE_IN_WITH != 0);
        }
        self.add_capture(row, Name(N_DOT_UNBOUND), false);
    }

    /// Adds a reference to `name` that a function compiled later leaves in the innermost scope. Nothing but that function
    /// is between the reference and the scope, so it comes in like a group that a nested scope passes on.
    fn add_capture(&mut self, row: u32, name: Name, used_inside_with_statement: bool) {
        let node = self.add_outer_reference(row, name);
        let frame = self.frames.last_mut().expect("there is an open scope");
        let mut group = Group::new(name, ScopedNameId::NONE);
        group.add_node(&mut self.links, node);
        group.used_inside_with_statement = used_inside_with_statement;
        frame.groups_from_nested_scopes.push(group);
    }

    /// The code of a class that runs as a function has no captures of its own: it is in the tree of the code around it,
    /// so that its free names are the groups that leave its scope.
    fn note_free_names_of_class_code(&mut self, row: u32, groups: &mut ZoneVec<'_, Group>) {
        let mut has_unbound_name = false;
        for group in groups.iter_mut() {
            has_unbound_name |= group.name == Name(N_DOT_UNBOUND);
            let node = u32_from_usize(self.resolutions.len());
            self.resolutions.push(Resolution::default());
            self.outer_references.push((row, group.name, node));
            group.add_node(&mut self.links, node);
        }
        if !has_unbound_name {
            let node = self.add_outer_reference(row, Name(N_DOT_UNBOUND));
            let mut group = Group::new(Name(N_DOT_UNBOUND), ScopedNameId::NONE);
            group.add_node(&mut self.links, node);
            groups.push(group);
        }
    }

    /// Resolves a group in a scope, and returns whether it goes on to the scope around it.
    fn resolve_group(&mut self, frame: &mut Frame<'z>, group: &mut Group) -> bool {
        let name = group.name;
        let flags = self.names.get(group.scoped_name).binding_flags;
        let is_function = frame.kind == FrameKind::Function;
        let is_program = frame.kind == FrameKind::Program;
        let is_catch = frame.kind == FrameKind::Catch;
        let is_class = frame.kind == FrameKind::Class;
        let is_top_level = frame.is_top_level();
        let arguments_name = Name(N_ARGUMENTS);
        let is_arguments_of_function = is_function && !frame.is_arrow && name == arguments_name;

        if flags & decl::CLASS_NAME != 0 && is_class {
            return false;
        }
        let mut is_local_kind = (is_top_level && flags & decl::VAR != 0)
            || (flags & decl::LEXICAL != 0 && !is_class)
            || flags & decl::FUNCTION != 0;
        is_local_kind |= is_arguments_of_function;
        let is_catch_parameter = is_catch && flags & (decl::SIMPLE_CATCH | decl::CATCH_PATTERN) != 0;
        is_local_kind |= is_catch_parameter;
        if flags & decl::REFERENCED_IN_PARAMETERS != 0 && flags & decl::PARAMETER == 0 {
            return false;
        }
        if is_function && flags & decl::FUNCTION_NAME != 0 {
            self.mark_inside_scope_with_eval(group);
        }
        let mut is_parameter = false;
        if is_function && flags & decl::PARAMETER != 0 {
            if frame.function_flags & function_flag::ARGUMENTS_ACCESS == 0 || frame.rest_name == Some(name) {
                is_parameter = true;
            } else {
                return false;
            }
        }
        if is_function && flags & decl::ANNEX_B != 0 {
            return false;
        }
        if is_program {
            let is_module_declaration = self.is_module && flags != 0;
            // NB: What stands for unbound names comes from functions all over the code, in a `with` or not. Which names
            //     are used in one is known by name.
            let used_in_with = (group.used_inside_with_statement && name != Name(N_DOT_UNBOUND))
                || flags & decl::USED_IN_WITH != 0
                || self.script.is_global_name_used_in_with(name);
            if !used_in_with && !is_module_declaration && !self.initiated_by_eval && !self.suppress_globals {
                for node in group.nodes(&self.links) {
                    let resolution = &mut self.resolutions[node as usize];
                    if !resolution.is_inside_scope_with_eval() {
                        resolution.set_is_global();
                    }
                }
            }
            return false;
        }
        if is_local_kind || is_parameter {
            if flags & decl::HOISTABLE != 0 {
                return false;
            }
            let captured = if name == arguments_name && flags == 0 {
                frame.function_flags & (function_flag::CAPTURES_ARGUMENTS | function_flag::CONTAINS_EVAL) != 0
            } else {
                flags & decl::CAPTURED != 0
            };
            if captured || group.used_inside_with_statement {
                return false;
            }
            let position = self.names.get(group.scoped_name).parameter_position;
            let binding = if is_parameter && position != NONE {
                if position > Resolution::MAX_LOCAL_INDEX {
                    return false;
                }
                LocalBinding {
                    local_type: LocalType::Argument,
                    index: position,
                }
            } else {
                let variable = if is_parameter {
                    LocalVariable {
                        name,
                        kind: LocalVarKind::PatternParameter,
                        is_mutable: true,
                        scope_offsets: None,
                        enclosing_loops: 0,
                    }
                } else {
                    let kind = if is_catch_parameter {
                        LocalVarKind::CatchClauseParameter
                    } else if is_arguments_of_function {
                        LocalVarKind::ArgumentsObject
                    } else if is_top_level && flags & decl::VAR != 0 {
                        LocalVarKind::Var
                    } else if flags & decl::LEXICAL != 0 {
                        LocalVarKind::LetOrConst
                    } else if flags & decl::FUNCTION != 0 {
                        LocalVarKind::Function
                    } else {
                        LocalVarKind::Var
                    };
                    LocalVariable {
                        name,
                        kind,
                        is_mutable: group.declaration_kind != Some(DeclarationKind::Const),
                        scope_offsets: frame.block_offsets,
                        enclosing_loops: 0,
                    }
                };
                let Some(index) = self.add_local_variable(frame, group, variable) else {
                    return false;
                };
                LocalBinding {
                    local_type: LocalType::Variable,
                    index,
                }
            };
            for node in group.nodes(&self.links) {
                self.resolutions[node as usize].set_local(binding);
            }
            self.names.get_mut(group.scoped_name).local = Some(binding);
            return false;
        }
        if frame.kind == FrameKind::With {
            group.used_inside_with_statement = true;
        }
        if frame.row_flags & SCOPE_ROW_EVAL_IN_FUNCTION != 0 {
            self.mark_inside_scope_with_eval(group);
        }
        true
    }

    fn mark_inside_scope_with_eval(&mut self, group: &Group) {
        for node in group.nodes(&self.links) {
            let resolution = &mut self.resolutions[node as usize];
            resolution.set_is_inside_scope_with_eval();
            resolution.set_is_inside_tree_scope_with_eval();
        }
    }

    /// Adds a local to the function or static block around a scope, or to the program if there is neither.
    fn add_local_variable(&mut self, frame: &mut Frame<'z>, group: &Group, mut variable: LocalVariable) -> Option<u32> {
        let holder = if frame.holds_locals() {
            frame
        } else if let Some(function) = self.frames.iter_mut().rev().find(|frame| frame.holds_locals()) {
            variable.enclosing_loops = frame.loop_depth - function.loop_depth;
            function
        } else {
            if group.declaration_kind == Some(DeclarationKind::Var) {
                return None;
            }
            let program = self.frames.first_mut().expect("there is a program scope");
            variable.enclosing_loops = frame.loop_depth - program.loop_depth;
            program
        };
        let list = &mut holder.local_variables;
        let index = u32_from_usize(list.len());
        if index > Resolution::MAX_LOCAL_INDEX {
            return None;
        }
        list.push(variable);
        Some(index)
    }

    // https://tc39.es/ecma262/#sec-block-level-function-declarations-web-legacy-compatibility-semantics
    fn hoist_functions(&mut self, frame: &mut Frame<'_>) {
        if frame.functions_to_hoist.is_empty() && frame.functions_hoisted_from_nested_scopes.is_empty() {
            return;
        }
        let mut functions = frame.functions_to_hoist.take();
        functions.append(&mut frame.functions_hoisted_from_nested_scopes);
        for &name in &functions {
            if self.names.blocks_hoisted_function(name, frame.depth) {
                continue;
            }
            if frame.is_top_level() {
                if self.names.binding(name, frame.depth) & decl::ANNEX_B_NAME != 0 {
                    let id = self.names.find_or_add(name, frame);
                    let entry = self.names.get_mut(id);
                    if !entry.is_in_annexb_function_names {
                        entry.is_in_annexb_function_names = true;
                        frame.annexb_function_names.push(name);
                    }
                }
            } else if let Some(parent) = self.frames.last_mut() {
                let parent_flags = self.names.binding(name, parent.depth);
                let parent_has_function = parent_flags & decl::FUNCTION != 0 && parent_flags & decl::HOISTABLE != 0;
                if !self.names.blocks_hoisted_function(name, parent.depth) && !parent_has_function {
                    parent.functions_hoisted_from_nested_scopes.push(name);
                }
            }
        }
        functions.clear();
        frame.functions_to_hoist = functions;
    }

    /// The name of the function that a statement declares, labelled or not.
    fn declared_function_name(&self, statement: u32) -> Option<Name> {
        match self.tag(statement) {
            tag::FUNCTION_DECL => Some(Name(self.b(statement))),
            tag::LABELED => self.declared_function_name(self.b(statement)),
            _ => None,
        }
    }

    // https://tc39.es/ecma262/#sec-functiondeclarationinstantiation
    fn build_function_scope_data(&mut self, frame: &mut Frame<'z>, children: &[u32], row: &mut ScopeRow) {
        for (index, &child) in children.iter().enumerate() {
            if let Some(name) = self.declared_function_name(child) {
                let id = self.names.find_or_add(name, frame);
                self.names.get_mut(id).last_function_position = u32_from_usize(index);
            }
        }

        for &name in &frame.variable_order {
            let entry = self
                .names
                .find(name, frame.depth)
                .expect("the scope knows its variables");
            let flags = entry.binding_flags;
            if flags & decl::VAR == 0 {
                continue;
            }
            let is_parameter = flags & decl::PARAMETER != 0;
            if entry.local.is_none() {
                row.non_local_var_count_for_parameter_expressions += 1;
                if !is_parameter {
                    row.non_local_var_count += 1;
                }
            }
        }

        let arguments_name = Name(N_ARGUMENTS);
        let arguments_entry = self.names.find(arguments_name, frame.depth);
        let arguments_flags = arguments_entry.map_or(0, |entry| entry.binding_flags);
        row.has_function_scope_data = true;
        row.has_function_named_arguments = arguments_entry.is_some_and(|entry| entry.last_function_position != NONE);
        row.has_argument_parameter = arguments_flags & decl::PARAMETER != 0;
        row.has_lexically_declared_arguments = arguments_flags & decl::LEXICAL != 0;
        let flags = frame.function_flags;
        row.uses_this = flags & function_flag::SCOPE_USES_THIS != 0;
        row.uses_this_from_environment = flags & function_flag::SCOPE_THIS_FROM_ENVIRONMENT != 0;
        row.contains_direct_call_to_eval = frame.row_flags & SCOPE_ROW_POISONED != 0;
        row.contains_access_to_arguments_object = flags & function_flag::ARGUMENTS_ACCESS != 0;
    }

    // === Programs ===

    fn program(&mut self, root: u32) {
        self.push_frame(FrameKind::Program, self.b(root));
        let list = self.list(self.a(root));
        self.statements(list);
        let row = self.close_frame(list);
        self.scopes.rows.push(row);
    }

    fn statements(&mut self, list: &[u32]) {
        for &statement in list {
            self.statement(statement);
        }
    }

    // === Statements ===

    fn block(&mut self, node: u32) {
        let (start, list, row) = (self.pos(node), self.a(node), self.b(node));
        self.push_frame(FrameKind::Block, row);
        self.top().block_offsets = Some((start, self.block_end(list)));
        self.statements(self.list(list));
        self.close_frame(&[]);
    }

    fn statement(&mut self, node: u32) {
        let (a, b) = (self.a(node), self.b(node));
        match self.tag(node) {
            tag::EMPTY | tag::DEBUGGER | tag::BREAK | tag::CONTINUE | tag::IMPORT => {}
            tag::EXPR_STMT | tag::THROW => self.expression(a),
            tag::SCOPED => self.for_statement(node),
            tag::FOR | tag::FOR_IN | tag::FOR_OF | tag::FOR_AWAIT_OF => {
                unreachable!("a loop of this kind always has a scope around it")
            }
            tag::IF_FUNCTION_BLOCK => {
                self.push_frame(FrameKind::Block, b);
                self.statement(a);
                self.close_frame(&[]);
            }
            tag::BLOCK => self.block(node),
            tag::IF => {
                let (consequent, alternate) = (self.extra(b), self.extra(b + 1));
                self.expression(a);
                self.statement(consequent);
                if alternate != 0 {
                    self.statement(alternate);
                }
            }
            tag::WHILE => {
                self.expression(a);
                self.loop_body(b);
            }
            tag::DO_WHILE => {
                self.loop_body(a);
                self.expression(b);
            }
            tag::SWITCH => self.switch_statement(node),
            tag::WITH => {
                let (body, row) = (self.extra(b), self.extra(b + 1));
                self.expression(a);
                self.note_creates_environment();
                self.push_frame(FrameKind::With, row);
                self.statement(body);
                self.close_frame(&[]);
            }
            tag::LABELED => self.statement(b),
            tag::RETURN => {
                if a != 0 {
                    self.expression(a);
                }
            }
            tag::TRY => self.try_statement(node),
            tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING => self.variable_declaration(node),
            tag::FUNCTION_DECL => self.function_declaration(a, b),
            tag::CLASS_DECL => {
                self.class(a);
                let name = self.a(a);
                if name != NONE {
                    self.register(a, Name(name), None);
                }
            }
            tag::EXPORT_ALL => {}
            tag::EXPORT_DEFAULT => {
                let inner_tag = self.tag(a);
                if inner_tag == tag::FUNCTION && b != 0 {
                    let name = self.script.index.name[self.b(a) as usize];
                    if name == NONE {
                        let default_name = self.script.default_export_name;
                        let id = self.names.find_or_add(default_name, &mut self.frames[0]);
                        let entry = self.names.get_mut(id);
                        entry.binding_flags = decl::LEXICAL;
                        entry.is_bound = true;
                    }
                    self.function_declaration(a, name);
                } else if inner_tag == tag::CLASS_DECL {
                    self.statement(a);
                } else {
                    self.expression(a);
                }
            }
            tag::EXPORT_NAMED => {
                if a == 0 {
                    self.statement(b);
                }
            }
            other => unreachable!("tag {other} is no statement"),
        }
    }

    fn loop_body(&mut self, body: u32) {
        self.loop_depth += 1;
        self.statement(body);
        self.loop_depth -= 1;
    }

    /// A loop, whose head the block around it scopes.
    fn for_statement(&mut self, node: u32) {
        let (inner, row) = (self.a(node), self.b(node));
        self.push_frame(FrameKind::ForLoop, row);
        let (a, b) = (self.a(inner), self.b(inner));
        if self.tag(inner) == tag::FOR {
            let (test, update, body) = (self.extra(b), self.extra(b + 1), self.extra(b + 2));
            if a != 0 {
                if self.is_declaration(a) {
                    self.statement(a);
                } else {
                    self.expression(a);
                }
            }
            if test != 0 {
                self.expression(test);
            }
            if update != 0 {
                self.expression(update);
            }
            self.loop_body(body);
        } else {
            let (right, body) = (self.extra(b), self.extra(b + 1));
            let is_pattern = matches!(self.tag(a), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN);
            if is_pattern {
                self.note_names_in_expression(a);
            } else if self.is_declaration(a) {
                self.statement(a);
            } else {
                self.expression(a);
            }
            self.expression(right);
            self.loop_body(body);
            // NB: The names of a pattern get resolved as if the pattern came after the loop's body.
            if is_pattern {
                let mut bound = self.take_bound_names();
                self.pattern(a, PatternMode::Assignment, &mut bound);
                for &name in &bound {
                    self.register(name.node, name.name, None);
                }
                self.give_back_bound_names(bound);
            }
        }
        self.close_frame(&[]);
    }

    fn is_declaration(&self, node: u32) -> bool {
        matches!(
            self.tag(node),
            tag::VAR | tag::LET | tag::CONST | tag::USING | tag::AWAIT_USING
        )
    }

    fn switch_statement(&mut self, node: u32) {
        let (a, b) = (self.a(node), self.b(node));
        let (cases, row) = (self.list(self.extra(b)), self.extra(b + 1));
        self.expression(a);
        self.push_frame(FrameKind::Block, row);
        for &case in cases {
            let (test, statements) = (self.a(case), self.list(self.b(case)));
            if test != 0 {
                self.expression(test);
            }
            self.statements(statements);
        }
        self.close_frame(&[]);
    }

    fn try_statement(&mut self, node: u32) {
        let (a, b) = (self.a(node), self.b(node));
        let (parameter, handler, finalizer) = (self.extra(b), self.extra(b + 1), self.extra(b + 2));
        let catch_scope = self.extra(b + 4);
        self.statement(a);
        if handler != 0 {
            self.push_frame(FrameKind::Catch, catch_scope);
            if parameter != 0 {
                if self.tag(parameter) == tag::IDENT {
                    self.register(parameter, Name(self.a(parameter)), None);
                } else {
                    let mut bound = self.take_bound_names();
                    self.pattern(parameter, PatternMode::Binding, &mut bound);
                    for &name in &bound {
                        self.register(name.node, name.name, None);
                    }
                    self.give_back_bound_names(bound);
                }
            }
            self.block(handler);
            self.close_frame(&[]);
        }
        if finalizer != 0 {
            self.statement(finalizer);
        }
    }

    fn variable_declaration(&mut self, node: u32) {
        let kind = match self.tag(node) {
            tag::VAR => DeclarationKind::Var,
            tag::LET => DeclarationKind::Let,
            _ => DeclarationKind::Const,
        };
        let is_var = kind == DeclarationKind::Var;
        for &declarator in self.list(self.a(node)) {
            let (target, init) = (self.a(declarator), self.b(declarator));
            if self.tag(target) == tag::IDENT {
                let name = Name(self.a(target));
                self.register(target, name, Some(kind));
                if is_var {
                    self.touch_var(name);
                }
            } else {
                let mut bound = self.take_bound_names();
                self.pattern(target, PatternMode::Binding, &mut bound);
                for &name in &bound {
                    self.register(name.node, name.name, Some(kind));
                    if is_var {
                        self.touch_var(name.name);
                    }
                }
                self.give_back_bound_names(bound);
            }
            if init != 0 {
                self.expression(init);
            }
        }
    }

    fn function_declaration(&mut self, function: u32, name: u32) {
        let name = if name == NONE {
            self.script.default_export_name
        } else {
            Name(name)
        };
        let binding = self.names.innermost_binding(name);
        self.register(function, name, None);
        let resolution = self.resolution(function);
        resolution.set_is_declared_function();
        if binding & decl::HOISTED != 0 {
            resolution.set_is_hoisted();
        }
        let is_module_top_level = self.is_module && self.top().kind == FrameKind::Program;
        if !self.top().is_top_level() {
            self.note_creates_environment();
        }
        let frame = self.frames.last_mut().expect("there is an open scope");
        if frame.is_top_level() {
            if !is_module_top_level {
                self.names.touch_variable(name, frame);
            }
        } else if self.names.binding(name, frame.depth) & decl::HOISTABLE != 0 {
            frame.functions_to_hoist.push(name);
        }
        self.function(function, Some(name));
    }

    // === Functions ===

    /// The function at `node`, whose name is `name` if it binds one for itself or a declaration binds one for it.
    fn function(&mut self, node: u32, name: Option<Name>) {
        let row = self.b(node);
        if self.a(node) != 0 {
            self.function_code(node, name);
        }
        self.note_free_names_of_function(row);
    }

    /// The function at `node`, whose code is in the tree.
    fn function_code(&mut self, node: u32, name: Option<Name>) {
        let tree = self.a(node);
        let row = self.b(node);
        let flags = self.script.index.flags[row as usize];
        let parameters = self.list(self.extra(tree));
        let body = self.extra(tree + 1);
        let scope_row = self.extra(tree + 3);
        let body_scope_row = self.extra(tree + 4);
        let is_expression = self.extra(tree + 6) != 0;

        let mut frame = self.new_frame(FrameKind::Function);
        frame.is_arrow = flags & function_flag::ARROW != 0;
        frame.function_flags = flags;
        self.merge_row(&mut frame, scope_row);
        self.merge_row(&mut frame, body_scope_row);
        for (position, &parameter) in parameters.iter().enumerate() {
            let target = match self.tag(parameter) {
                tag::REST | tag::ASSIGN_PATTERN | tag::ASSIGN_BASE => self.a(parameter),
                _ => parameter,
            };
            if self.tag(target) == tag::IDENT {
                let parameter_name = Name(self.a(target));
                let id = self.names.find_or_add(parameter_name, &mut frame);
                self.names.get_mut(id).parameter_position = u32_from_usize(position);
                if self.tag(parameter) == tag::REST {
                    frame.rest_name = Some(parameter_name);
                }
            }
        }
        if let Some(name) = name {
            self.names.touch_variable(name, &mut frame);
        }
        self.frames.push(frame);

        self.parameters(parameters);

        let scope = if is_expression {
            self.expression(body);
            self.close_frame(&[])
        } else {
            let statements = self.list(body);
            self.statements(statements);
            self.close_frame(statements)
        };
        self.function_scope_rows
            .push((node, u32_from_usize(self.scopes.rows.len())));
        self.scopes.rows.push(scope);
    }

    /// The parameters of the function of the innermost scope. Their names come after everything in them that the
    /// function refers to.
    fn parameters(&mut self, parameters: &[u32]) {
        let mut bound = self.take_bound_names();
        let mut has_parameter_expressions = false;
        for &parameter in parameters {
            let (target, default) = match self.tag(parameter) {
                tag::REST => (self.a(parameter), 0),
                tag::ASSIGN_PATTERN | tag::ASSIGN_BASE => (self.a(parameter), self.b(parameter)),
                _ => (parameter, 0),
            };
            if self.tag(target) == tag::IDENT {
                bound.push(BoundName {
                    node: target,
                    name: Name(self.a(target)),
                });
            } else {
                self.pattern(target, PatternMode::Binding, &mut bound);
                has_parameter_expressions |= self.tree.pattern_contains_expression(target);
            }
            if default != 0 {
                self.expression(default);
                has_parameter_expressions = true;
            }
        }
        let frame = self.frames.last_mut().expect("there is an open scope");
        for name in &bound {
            let group = self.names.group_for(name.name, frame);
            group.add_node(&mut self.links, name.node);
            self.names.touch_variable(name.name, frame);
            let id = self.names.find_or_add(name.name, frame);
            self.names.get_mut(id).is_bound_by_parameter_list = true;
        }
        if has_parameter_expressions {
            for index in 0..frame.groups.len() {
                let group = frame.groups[index];
                if !self.names.get(group.scoped_name).is_bound_by_parameter_list {
                    self.names.touch_variable(group.name, frame);
                }
            }
        }
        self.give_back_bound_names(bound);
    }

    // === Classes ===

    fn class(&mut self, node: u32) {
        let extra = self.b(node);
        let super_class = self.extra(extra);
        let elements = self.list(self.extra(extra + 1));
        let row = self.extra(extra + 2);
        let constructor = self.extra(extra + 3);

        self.note_creates_environment();
        self.push_frame(FrameKind::Class, row);
        if super_class != 0 {
            self.expression(super_class);
        }
        for &element in elements {
            if self.tag(element) == tag::METHOD && self.b(element) == constructor && constructor != 0 {
                self.expression(constructor);
                continue;
            }
            self.class_element(element);
        }
        self.close_frame(&[]);
    }

    fn class_element(&mut self, element: u32) {
        let inner = if self.tag(element) == tag::STATIC_ELEMENT {
            self.a(element)
        } else {
            element
        };
        let (a, b) = (self.a(inner), self.b(inner));
        let inner_tag = self.tag(inner);
        match inner_tag {
            tag::CLASS_FIELD | tag::COMPUTED_CLASS_FIELD => {
                if inner_tag == tag::COMPUTED_CLASS_FIELD {
                    self.expression(a);
                }
                if b != 0 {
                    self.field_initializer(b);
                }
            }
            tag::CLASS_STATIC_BLOCK => self.static_block(inner),
            other => {
                if matches!(
                    other,
                    tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER
                ) {
                    self.expression(a);
                }
                self.expression(b);
            }
        }
    }

    fn field_initializer(&mut self, initializer: u32) {
        let (value, extra) = (self.a(initializer), self.b(initializer));
        self.push_frame(FrameKind::ClassField, self.extra(extra));
        self.top().code_of_class_row = Some(self.extra(extra + 1));
        self.expression(value);
        let row = self.close_frame(&[]);
        *self.resolution(initializer) = Resolution(u32::from(row.creates_environment));
    }

    fn static_block(&mut self, block: u32) {
        let (a, b) = (self.a(block), self.b(block));
        self.push_frame(FrameKind::ClassStaticInitializer, self.extra(b));
        self.top().code_of_class_row = Some(self.extra(b + 1));
        let statements = self.list(a);
        self.statements(statements);
        let row = self.close_frame(statements);
        *self.resolution(block) = Resolution(u32_from_usize(self.scopes.rows.len()));
        self.scopes.rows.push(row);
    }

    // === Patterns ===

    /// Makes room for the names that an assignment target refers to as an expression, which come before the names
    /// that only it as a pattern refers to.
    fn note_names_in_expression(&mut self, node: u32) {
        let start = self.chain_links.len();
        let mut first = node;
        while Self::continues_chain_in_target(self.tag(first)) {
            self.chain_links.push(first);
            first = self.a(first);
        }
        self.note_names_in_operand(first);
        while self.chain_links.len() > start {
            let link = self.chain_links.pop().expect("a link was pushed");
            let b = self.b(link);
            match self.tag(link) {
                tag::MEMBER | tag::OPT_MEMBER | tag::PRIVATE_MEMBER | tag::OPT_PRIVATE_MEMBER => {}
                tag::CALL | tag::OPT_CALL | tag::NEW => {
                    for &item in self.list(b) {
                        self.note_names_in_expression(item);
                    }
                }
                _ => self.note_names_in_expression(b),
            }
        }
    }

    /// Whether [`Resolver::note_names_in_expression()`] gets to a node of this tag through a loop.
    fn continues_chain_in_target(node_tag: u8) -> bool {
        matches!(
            node_tag,
            tag::MEMBER
                | tag::OPT_MEMBER
                | tag::PRIVATE_MEMBER
                | tag::OPT_PRIVATE_MEMBER
                | tag::CALL
                | tag::OPT_CALL
                | tag::NEW
                | tag::INDEX
                | tag::OPT_INDEX
                | tag::TAGGED_TEMPLATE
        ) || node_tag >= tag::ASSIGN_BASE
    }

    fn note_names_in_operand(&mut self, node: u32) {
        let (a, b) = (self.a(node), self.b(node));
        let node_tag = self.tag(node);
        match node_tag {
            tag::IDENT => {
                let frame = self.frames.last_mut().expect("there is an open scope");
                self.names.group_for(Name(a), frame);
            }
            tag::SHORTHAND => {
                let frame = self.frames.last_mut().expect("there is an open scope");
                self.names.group_for(Name(a), frame);
                if matches!(self.tag(b), tag::ASSIGN_PATTERN | tag::ASSIGN_BASE) {
                    self.note_names_in_expression(self.b(b));
                }
            }
            tag::ARRAY | tag::OBJECT | tag::SEQUENCE | tag::TEMPLATE | tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => {
                for &item in self.list(a) {
                    self.note_names_in_expression(item);
                }
            }
            tag::CONDITIONAL => {
                self.note_names_in_expression(a);
                self.note_names_in_expression(self.extra(b));
                self.note_names_in_expression(self.extra(b + 1));
            }
            tag::PROPERTY | tag::METHOD | tag::GETTER | tag::SETTER => self.note_names_in_expression(b),
            tag::CHAIN
            | tag::PAREN
            | tag::SPREAD
            | tag::YIELD
            | tag::YIELD_STAR
            | tag::AWAIT
            | tag::UNARY
            | tag::PREFIX_UPDATE
            | tag::POSTFIX_UPDATE
            | tag::REST => {
                if a != 0 {
                    self.note_names_in_expression(a);
                }
            }
            tag::COMPUTED_PROPERTY
            | tag::COMPUTED_METHOD
            | tag::COMPUTED_GETTER
            | tag::COMPUTED_SETTER
            | tag::ASSIGN_PATTERN => {
                self.note_names_in_expression(a);
                self.note_names_in_expression(b);
            }
            tag::IMPORT_CALL => {
                self.note_names_in_expression(a);
                if b != 0 {
                    self.note_names_in_expression(b);
                }
            }
            _ => {}
        }
    }

    fn pattern(&mut self, node: u32, mode: PatternMode, bound: &mut ZoneVec<'_, BoundName>) {
        let is_array = self.tag(node) == tag::ARRAY_PATTERN;
        for &item in self.list(self.a(node)) {
            self.pattern_entry(item, is_array, mode, bound);
        }
    }

    /// The target of a pattern's entry, which is an alias of the entry's name.
    fn pattern_alias(&mut self, value: u32, mode: PatternMode, bound: &mut ZoneVec<'_, BoundName>) {
        match self.tag(value) {
            tag::ARRAY_PATTERN | tag::OBJECT_PATTERN => self.pattern(value, mode, bound),
            tag::IDENT if mode == PatternMode::Binding => bound.push(BoundName {
                node: value,
                name: Name(self.a(value)),
            }),
            _ => self.expression(value),
        }
    }

    fn pattern_entry(&mut self, item: u32, is_array: bool, mode: PatternMode, bound: &mut ZoneVec<'_, BoundName>) {
        let item_tag = self.tag(item);
        if item_tag == tag::HOLE {
            return;
        }
        let is_rest = item_tag == tag::REST;
        let mut value = match item_tag {
            tag::REST => self.a(item),
            tag::PROPERTY | tag::COMPUTED_PROPERTY | tag::SHORTHAND => self.b(item),
            _ => item,
        };
        let mut initializer = 0;
        if matches!(self.tag(value), tag::ASSIGN_PATTERN | tag::ASSIGN_BASE) {
            initializer = self.b(value);
            value = self.a(value);
        }

        if is_array {
            self.pattern_alias(value, mode, bound);
        } else if item_tag == tag::SHORTHAND {
            let name = Name(self.a(item));
            self.register(item, name, None);
            bound.push(BoundName { node: item, name });
        } else if is_rest {
            if mode == PatternMode::Binding {
                let name = Name(self.a(value));
                self.register(value, name, None);
                bound.push(BoundName { node: value, name });
            } else {
                self.expression(value);
            }
        } else {
            if item_tag == tag::COMPUTED_PROPERTY {
                self.expression(self.a(item));
            }
            self.pattern_alias(value, mode, bound);
        }
        if initializer != 0 {
            self.expression(initializer);
        }
    }

    // === Expressions ===

    fn arguments(&mut self, list: u32) {
        for &argument in self.list(list) {
            let is_spread = self.tag(argument) == tag::SPREAD;
            self.expression(if is_spread { self.a(argument) } else { argument });
        }
    }

    /// Walks an expression. The parser makes chains like `a + b + c`, `a.b.c` and `a()()` in a loop, which puts no limit
    /// on their length, so that they get a loop here as well. What is left is as deep as the recursion of the parser was.
    fn expression(&mut self, node: u32) {
        let start = self.chain_links.len();
        let mut first = node;
        while self.continues_chain(first) {
            self.chain_links.push(first);
            first = self.a(first);
        }
        self.operand(first);
        while self.chain_links.len() > start {
            let link = self.chain_links.pop().expect("a link was pushed");
            let b = self.b(link);
            match self.tag(link) {
                tag::MEMBER | tag::PRIVATE_MEMBER => {}
                tag::CALL => self.arguments(b),
                tag::TAGGED_TEMPLATE => self.template(b),
                _ => self.expression(b),
            }
        }
    }

    /// Whether the first operand of `node` comes before the rest of it, and is something that gets walked.
    fn continues_chain(&self, node: u32) -> bool {
        match self.tag(node) {
            tag::MEMBER | tag::PRIVATE_MEMBER | tag::INDEX | tag::TAGGED_TEMPLATE => true,
            tag::CALL => self.tag(self.a(node)) != tag::SUPER,
            node_tag => node_tag >= tag::BINARY_BASE,
        }
    }

    fn operand(&mut self, node: u32) {
        let node_tag = self.tag(node);
        let (a, b) = (self.a(node), self.b(node));
        match node_tag {
            tag::IDENT => self.register(node, Name(a), None),
            tag::NUM
            | tag::BIGINT
            | tag::STR
            | tag::REGEX
            | tag::TRUE
            | tag::FALSE
            | tag::NULL
            | tag::THIS
            | tag::SUPER
            | tag::NEW_TARGET
            | tag::IMPORT_META
            | tag::PRIVATE_NAME => {}
            tag::FUNCTION => {
                let flags = self.script.index.flags[b as usize];
                let is_method = flags & function_flag::METHOD != 0;
                let name = self.script.index.name[b as usize];
                if !is_method && name != NONE && flags & function_flag::NAME_UNUSED == 0 {
                    self.note_creates_environment();
                }
                self.function(node, (!is_method && name != NONE).then_some(Name(name)));
            }
            tag::PAREN | tag::SPREAD | tag::AWAIT | tag::UNARY | tag::PREFIX_UPDATE | tag::POSTFIX_UPDATE => {
                self.expression(a);
            }
            tag::CHAIN => self.optional_chain(a),
            tag::YIELD | tag::YIELD_STAR => {
                if a != 0 {
                    self.expression(a);
                }
            }
            tag::CONDITIONAL => {
                let (consequent, alternate) = (self.extra(b), self.extra(b + 1));
                self.expression(a);
                self.expression(consequent);
                self.expression(alternate);
            }
            tag::IMPORT_CALL => {
                self.expression(a);
                if b != 0 {
                    self.expression(b);
                }
            }
            tag::CALL => self.arguments(b),
            tag::NEW => {
                self.expression(a);
                self.arguments(b);
            }
            tag::ARRAY => {
                for &element in self.list(a) {
                    if self.tag(element) != tag::HOLE {
                        self.expression(element);
                    }
                }
            }
            tag::OBJECT => {
                for &property in self.list(a) {
                    self.object_property(property);
                }
            }
            tag::SEQUENCE => {
                for &item in self.list(a) {
                    self.expression(item);
                }
            }
            tag::TEMPLATE => self.template(node),
            tag::CLASS => self.class(node),
            tag::ASSIGN_PATTERN => self.assignment(a, b),
            _ if node_tag >= tag::ASSIGN_BASE => self.assignment(a, b),
            other => unreachable!("tag {other} is no expression"),
        }
    }

    fn template(&mut self, node: u32) {
        for &item in self.list(self.a(node)) {
            if self.tag(item) != tag::TEMPLATE_CHUNK {
                self.expression(item);
            }
        }
    }

    fn assignment(&mut self, a: u32, b: u32) {
        if matches!(self.tag(a), tag::ARRAY_PATTERN | tag::OBJECT_PATTERN) {
            self.note_names_in_expression(a);
            let mut bound = self.take_bound_names();
            self.pattern(a, PatternMode::Assignment, &mut bound);
            for &name in &bound {
                self.register(name.node, name.name, None);
            }
            self.give_back_bound_names(bound);
        } else {
            self.expression(a);
        }
        self.expression(b);
    }

    fn object_property(&mut self, property: u32) {
        let property_tag = self.tag(property);
        let (a, b) = (self.a(property), self.b(property));
        match property_tag {
            tag::SPREAD => self.expression(a),
            tag::SHORTHAND => self.register(property, Name(a), None),
            _ => {
                if matches!(
                    property_tag,
                    tag::COMPUTED_PROPERTY | tag::COMPUTED_METHOD | tag::COMPUTED_GETTER | tag::COMPUTED_SETTER
                ) {
                    self.expression(a);
                }
                self.expression(b);
            }
        }
    }

    fn optional_chain(&mut self, node: u32) {
        let start = self.chain_links.len();
        let tree = self.tree;
        let base = tree.optional_chain(node, |link| self.chain_links.push(link));
        self.expression(base);
        // NB: The links go innermost first.
        while self.chain_links.len() > start {
            let link = self.chain_links.pop().expect("a link was pushed");
            let b = self.b(link);
            match self.tag(link) {
                tag::CALL | tag::OPT_CALL => self.arguments(b),
                tag::INDEX | tag::OPT_INDEX => self.expression(b),
                _ => {}
            }
        }
    }
}

fn append_to_column<T: Copy>(column: &mut ZoneVec<'_, T>, items: &[T]) -> ColumnRange {
    let start = u32_from_usize(column.len());
    column.extend_from_slice(items);
    ColumnRange {
        start,
        end: u32_from_usize(column.len()),
    }
}
