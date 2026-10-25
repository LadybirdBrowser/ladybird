/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Ports of the LibJSGC clang plugin checks to libjs_rust's GC.
//!
//! The collector scans the stack conservatively and traces cells precisely, so a `Gc` or `Value` in a local needs no
//! root. Heap memory that is not a cell is never scanned: a GC pointer that a local owns through a `Vec`, `Box`,
//! `HashMap`, boxed closure and so on can be collected while the local still uses it.

use rustc_data_structures::fx::{FxHashMap, FxHashSet};
use rustc_errors::DiagDecorator;
use rustc_hir::def::DefKind;
use rustc_hir::def_id::{DefId, LOCAL_CRATE, LocalDefId};
use rustc_hir::intravisit::FnKind;
use rustc_hir::{Body, ByRef, Expr, ExprKind, FnDecl, FnRetTy, HirId, Item, ItemKind, Node, Pat, PatKind};
use rustc_lint::{LateContext, LateLintPass, Lint};
use rustc_middle::lint::LintLevelSource;
use rustc_middle::ty::adjustment::{Adjust, PointerCoercion};
use rustc_middle::ty::print::with_forced_trimmed_paths;
use rustc_middle::ty::{self, Ty, TyCtxt};
use rustc_session::{declare_lint, impl_lint_pass};

use rustc_span::{DesugaringKind, Span};

declare_lint! {
    /// A local, parameter or return value owns GC pointers through heap memory that the collector does not scan.
    ///
    /// Use `MarkedVec` (or a `Root`) for such temporaries, or keep them in a traced cell. When the GC pointers are
    /// kept alive some other way, say so:
    /// `#[cfg_attr(ladybird_gc_lints, expect(gc_container_on_stack, reason = "..."))]`.
    pub GC_CONTAINER_ON_STACK,
    Deny,
    "GC pointers kept in heap memory that is not scanned for roots"
}

declare_lint! {
    /// A type that implements `Trace` holds a `Root`. A traced value is reachable from the heap, so its root keeps
    /// alive whatever reaches the value back, which leaks the cycle.
    pub ROOT_IN_TRACED_TYPE,
    Deny,
    "`Root` held by a type that implements `Trace`"
}

pub static LINTS: &[&Lint] = &[GC_CONTAINER_ON_STACK, ROOT_IN_TRACED_TYPE];

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    /// A type of the crate that implements `Trace` by hand, like `Gc<T>`, `Value` or `PropertyKey`: it can hold GC
    /// pointers that its fields do not show, as `PropertyKey` does in a tagged word, and has to trace them to keep them
    /// alive.
    GcPointer,
    /// A type that implements `KeepsAlive`, like `Root`, `MarkedVec` or `Deferred`: whatever these hold stays alive.
    Rooted,
    /// A type that implements `SweptAfterCollection`, whose pointers do not keep cells alive, or `HoldsNoCells`.
    Weak,
    /// A container whose storage the type structure does not show as owned (`SmallVec` keeps its spill behind a
    /// pointer, with no `PhantomData`).
    HeapContainer,
    /// A type that implements `DefersCollections`, like `DeferGc`, which keeps the heap from collecting while it lives.
    DeferGc,
    Other,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Target {
    /// GC pointers that live in unscanned heap memory.
    UnscannedGcPointer,
    /// Roots anywhere in a value.
    Root,
}

#[derive(Default)]
pub struct GcLints<'tcx> {
    kinds: FxHashMap<DefId, Kind>,
    traced_types: FxHashSet<DefId>,
    cell_types: FxHashSet<DefId>,
    swept_types: FxHashSet<DefId>,
    holds_no_cells_types: FxHashSet<DefId>,
    keeps_alive_types: FxHashSet<DefId>,
    defers_collections_types: FxHashSet<DefId>,
    /// Whether each type has a field that defers collections, which its values cannot outlive.
    deferral_scoped: FxHashMap<DefId, bool>,
    reaches_gc_pointer: FxHashMap<Ty<'tcx>, bool>,
    reported_attributes: FxHashSet<Span>,
    hand_traced_types: FxHashSet<DefId>,
    trace_trait: Option<DefId>,
    enabled: bool,
}

impl_lint_pass!(GcLints<'_> => [
    GC_CONTAINER_ON_STACK,
    ROOT_IN_TRACED_TYPE,
]);

const LIBJS_CRATE: &str = "libjs_rust";

fn def_path(tcx: TyCtxt<'_>, def_id: DefId) -> String {
    let mut path = tcx.crate_name(def_id.krate).to_string();
    for component in tcx.def_path(def_id).data {
        if let Some(name) = component.data.get_opt_name() {
            path.push_str("::");
            path.push_str(name.as_str());
        }
    }
    path
}

fn normalize<'tcx>(cx: &LateContext<'tcx>, ty: Ty<'tcx>) -> Ty<'tcx> {
    let ty = cx.tcx.erase_and_anonymize_regions(ty);
    cx.tcx
        .try_normalize_erasing_regions(cx.typing_env(), ty::Unnormalized::new(ty))
        .unwrap_or(ty)
}

impl<'tcx> GcLints<'tcx> {
    fn kind(&mut self, tcx: TyCtxt<'_>, def_id: DefId) -> Kind {
        if self.swept_types.contains(&def_id) || self.holds_no_cells_types.contains(&def_id) {
            return Kind::Weak;
        }
        if self.keeps_alive_types.contains(&def_id) {
            return Kind::Rooted;
        }
        if self.defers_collections_types.contains(&def_id) {
            return Kind::DeferGc;
        }
        if self.hand_traced_types.contains(&def_id) {
            return Kind::GcPointer;
        }
        *self
            .kinds
            .entry(def_id)
            .or_insert_with(|| match def_path(tcx, def_id).as_str() {
                "smallvec::SmallVec" => Kind::HeapContainer,
                _ => Kind::Other,
            })
    }

    fn is_leaf(&mut self, tcx: TyCtxt<'_>, ty: Ty<'_>, target: Target) -> bool {
        match ty.kind() {
            ty::Adt(adt, _) => {
                let kind = self.kind(tcx, adt.did());
                match target {
                    Target::UnscannedGcPointer => kind == Kind::GcPointer,
                    Target::Root => kind == Kind::Rooted,
                }
            }
            // A reference to a cell is a GC pointer too, like a raw GC::Cell* in C++.
            ty::Ref(_, pointee, _) | ty::RawPtr(pointee, _) => {
                target == Target::UnscannedGcPointer
                    && matches!(pointee.kind(), ty::Adt(adt, _) if self.cell_types.contains(&adt.did()))
            }
            // A trait object that implements Trace traces what it hides, like a type that implements Trace by hand.
            ty::Dynamic(predicates, ..) => {
                target == Target::UnscannedGcPointer
                    && predicates.principal_def_id().is_some_and(|principal| {
                        rustc_middle::ty::elaborate::supertrait_def_ids(tcx, principal)
                            .any(|supertrait| Some(supertrait) == self.trace_trait)
                    })
            }
            _ => false,
        }
    }

    /// The types `ty` holds by value, with whether the holding owns them through heap memory.
    fn components(&mut self, cx: &LateContext<'tcx>, ty: Ty<'tcx>, target: Target) -> Vec<(Ty<'tcx>, bool)> {
        let tcx = cx.tcx;
        let normalize = |ty| normalize(cx, ty);
        match ty.kind() {
            ty::Adt(adt, _) if self.is_deferral_scoped(tcx, adt.did()) => Vec::new(),
            ty::Adt(adt, args) => match self.kind(tcx, adt.did()) {
                Kind::Weak | Kind::DeferGc => Vec::new(),
                // Everything a root holds is traced, but a root found inside one is still a root.
                Kind::Rooted if target == Target::UnscannedGcPointer => Vec::new(),
                Kind::Rooted | Kind::HeapContainer => args.types().map(|ty| (normalize(ty), true)).collect(),
                Kind::Other if adt.is_phantom_data() => vec![(normalize(args.type_at(0)), true)],
                Kind::GcPointer | Kind::Other => adt
                    .all_fields()
                    .map(|field| (normalize(field.ty(tcx, args).skip_normalization()), false))
                    .collect(),
            },
            ty::Tuple(types) => types.iter().map(|ty| (ty, false)).collect(),
            // An empty array holds nothing, which `Box::new([])` coerced to a boxed slice relies on.
            ty::Array(_, length) if length.try_to_target_usize(tcx) == Some(0) => Vec::new(),
            ty::Array(element, _) | ty::Slice(element) | ty::Pat(element, _) => vec![(*element, false)],
            ty::Closure(_, args) => args
                .as_closure()
                .upvar_tys()
                .iter()
                .map(|ty| (normalize(ty), false))
                .collect(),
            _ => Vec::new(),
        }
    }

    /// Whether any value of `ty` can hold a GC pointer, wherever it lives. Recursive types are cut at the cycle.
    fn reaches_gc_pointer(&mut self, cx: &LateContext<'tcx>, ty: Ty<'tcx>, in_progress: &mut Vec<Ty<'tcx>>) -> bool {
        if let Some(&reaches) = self.reaches_gc_pointer.get(&ty) {
            return reaches;
        }
        if self.is_leaf(cx.tcx, ty, Target::UnscannedGcPointer) {
            self.reaches_gc_pointer.insert(ty, true);
            return true;
        }
        if in_progress.contains(&ty) {
            return false;
        }
        in_progress.push(ty);
        let reaches = self
            .components(cx, ty, Target::UnscannedGcPointer)
            .into_iter()
            .any(|(component, _)| self.reaches_gc_pointer(cx, component, in_progress));
        in_progress.pop();
        // A negative answer reached while an enclosing type was still in progress may be cut short.
        if reaches || in_progress.is_empty() {
            self.reaches_gc_pointer.insert(ty, reaches);
        }
        reaches
    }

    /// Finds a `target` leaf in `ty`, returning it with the outermost container that holds it on the heap (for roots,
    /// the type itself).
    fn find(
        &mut self,
        cx: &LateContext<'tcx>,
        ty: Ty<'tcx>,
        target: Target,
        ancestors: &mut Vec<Ty<'tcx>>,
        heap_owner: Option<usize>,
    ) -> Option<(Ty<'tcx>, Ty<'tcx>)> {
        if self.is_leaf(cx.tcx, ty, target) {
            match target {
                Target::Root => return Some((ty, ty)),
                // NB: Held by value, a type that traces itself can still keep GC pointers in its own containers.
                Target::UnscannedGcPointer => {
                    if let Some(owner) = heap_owner {
                        return Some((ty, self.container(cx, ancestors, owner)));
                    }
                }
            }
        }
        if target == Target::UnscannedGcPointer && !self.reaches_gc_pointer(cx, ty, &mut Vec::new()) {
            return None;
        }
        if ancestors.contains(&ty) {
            return None;
        }
        ancestors.push(ty);
        let index = ancestors.len() - 1;
        let mut found = None;
        // The type that holds a PhantomData owns the storage it marks.
        let storage_owner = match ty.kind() {
            ty::Adt(adt, _) if adt.is_phantom_data() => index.saturating_sub(1),
            _ => index,
        };
        for (component, owned_on_heap) in self.components(cx, ty, target) {
            let owner = if owned_on_heap {
                heap_owner.or(Some(storage_owner))
            } else {
                heap_owner
            };
            found = self.find(cx, component, target, ancestors, owner);
            if found.is_some() {
                break;
            }
        }
        ancestors.pop();
        found
    }

    /// Names the container for a diagnostic: the innermost public type around the storage, so `Vec<Gc<T>>` rather
    /// than the `RawVec<Gc<T>>` that owns it.
    fn container(&self, cx: &LateContext<'tcx>, ancestors: &[Ty<'tcx>], owner: usize) -> Ty<'tcx> {
        let is_public = |ty: Ty<'tcx>| match ty.kind() {
            ty::Adt(adt, _) if adt.did().is_local() => true,
            ty::Adt(adt, _) => {
                cx.tcx.visible_parent_map(()).contains_key(&adt.did())
                    && !cx
                        .tcx
                        .lookup_stability(adt.did())
                        .is_some_and(|stability| stability.is_unstable())
            }
            _ => true,
        };
        let mut index = owner;
        while index > 0 && !is_public(ancestors[index]) {
            index -= 1;
        }
        ancestors[index]
    }

    /// The outermost container of `ty` that keeps GC pointers in unscanned heap memory.
    fn unscanned_container(&mut self, cx: &LateContext<'tcx>, ty: Ty<'tcx>) -> Option<Ty<'tcx>> {
        let ty = normalize(cx, ty);
        self.find(cx, ty, Target::UnscannedGcPointer, &mut Vec::new(), None)
            .map(|(_, container)| container)
    }

    /// Whether the type has a field that defers collections, like a `&DeferGc` or a `Deferred`, which its values
    /// cannot outlive, so that nothing collects while one lives.
    fn is_deferral_scoped(&mut self, tcx: TyCtxt<'_>, def_id: DefId) -> bool {
        if let Some(&scoped) = self.deferral_scoped.get(&def_id) {
            return scoped;
        }
        let adt = tcx.adt_def(def_id);
        let scoped = adt.is_struct()
            && adt.all_fields().any(|field| {
                let ty = tcx.type_of(field.did).instantiate_identity().skip_normalization();
                let held = match ty.kind() {
                    ty::Ref(_, pointee, _) => *pointee,
                    _ => ty,
                };
                matches!(held.kind(), ty::Adt(held, _) if self.defers_collections_types.contains(&held.did()))
            });
        self.deferral_scoped.insert(def_id, scoped);
        scoped
    }

    fn is_defer_gc_reference(&mut self, tcx: TyCtxt<'_>, ty: Ty<'_>) -> bool {
        matches!(ty.kind(), ty::Ref(_, pointee, _)
            if matches!(pointee.kind(), ty::Adt(adt, _) if self.kind(tcx, adt.did()) == Kind::DeferGc))
    }

    /// Whether the function takes a `&DeferGc`, which its caller keeps alive for as long as the function runs.
    fn takes_defer_gc(&mut self, cx: &LateContext<'tcx>, def_id: LocalDefId) -> bool {
        if !matches!(cx.tcx.def_kind(def_id), DefKind::Fn | DefKind::AssocFn) {
            return false;
        }
        let signature = cx.tcx.fn_sig(def_id).instantiate_identity().skip_normalization();
        signature
            .inputs()
            .skip_binder()
            .iter()
            .any(|&input| self.is_defer_gc_reference(cx.tcx, input))
    }

    /// Whether the closure takes a `&DeferGc`, like the one Heap::allocate() builds a cell with.
    fn closure_takes_defer_gc(&mut self, cx: &LateContext<'tcx>, def_id: LocalDefId) -> bool {
        let ty::Closure(_, args) = cx
            .tcx
            .type_of(def_id)
            .instantiate_identity()
            .skip_normalization()
            .kind()
        else {
            return false;
        };
        // NB: A closure's signature takes its arguments as one tuple.
        let inputs = args.as_closure().sig().inputs().skip_binder();
        inputs.iter().any(|&input| match input.kind() {
            ty::Tuple(types) => types.iter().any(|ty| self.is_defer_gc_reference(cx.tcx, ty)),
            _ => self.is_defer_gc_reference(cx.tcx, input),
        })
    }

    /// Whether `hir_id` is in the body of a function or closure that takes a `&DeferGc`, which outlives the body. A
    /// guard bound in a body proves nothing, as it can be dropped before what it should cover.
    fn in_defer_gc_scope(&mut self, cx: &LateContext<'tcx>, hir_id: HirId) -> bool {
        for (parent, node) in cx.tcx.hir_parent_iter(hir_id) {
            match node {
                Node::Expr(Expr {
                    kind: ExprKind::Closure(closure),
                    ..
                }) if self.closure_takes_defer_gc(cx, closure.def_id) => return true,
                Node::Item(_) | Node::ImplItem(_) | Node::TraitItem(_) => {
                    return self.takes_defer_gc(cx, parent.owner.def_id);
                }
                _ => {}
            }
        }
        false
    }

    fn emit(&mut self, cx: &LateContext<'_>, lint: &'static Lint, hir_id: HirId, span: Span, message: String) {
        let spec = cx.tcx.lint_level_spec_at_node(lint, hir_id);
        if (spec.is_allow() || spec.is_expect())
            && let LintLevelSource::Node {
                span: attribute_span,
                reason: None,
                ..
            } = spec.src
            && self.reported_attributes.insert(attribute_span)
        {
            cx.tcx.dcx().span_err(
                attribute_span,
                format!("allowing `{}` needs a `reason = \"...\"`", lint.name_lower()),
            );
        }
        cx.tcx.emit_node_span_lint(
            lint,
            hir_id,
            span,
            DiagDecorator(|diag| {
                diag.primary_message(message);
            }),
        );
    }

    fn check_unscanned(&mut self, cx: &LateContext<'tcx>, ty: Ty<'tcx>, hir_id: HirId, span: Span, what: &str) {
        if let Some(container) = self.unscanned_container(cx, ty) {
            self.emit(
                cx,
                GC_CONTAINER_ON_STACK,
                hir_id,
                span,
                with_forced_trimmed_paths!(format!(
                    "{what} '{container}' keeps GC pointers in heap memory that is not scanned for roots"
                )),
            );
        }
    }
}

impl<'tcx> LateLintPass<'tcx> for GcLints<'tcx> {
    fn check_crate(&mut self, cx: &LateContext<'tcx>) {
        let tcx = cx.tcx;
        let libjs = if tcx.crate_name(LOCAL_CRATE).as_str() == LIBJS_CRATE {
            Some(LOCAL_CRATE)
        } else {
            tcx.crates(())
                .iter()
                .copied()
                .find(|&krate| tcx.crate_name(krate).as_str() == LIBJS_CRATE)
        };
        let Some(libjs) = libjs else {
            return;
        };
        self.enabled = true;

        // Only implementations in the type's own crate count: libjs_rust implements Trace for Vec, Box and the like.
        let collect = |path: &str, into: &mut FxHashSet<DefId>| {
            let Some(trait_def_id) = tcx
                .all_traits_including_private()
                .find(|&def_id| def_id.krate == libjs && def_path(tcx, def_id) == path)
            else {
                tcx.dcx().fatal(format!(
                    "the Rust GC lints find GC types through `{path}`, which libjs_rust no longer has"
                ));
            };
            for implementation in tcx.all_impls(trait_def_id) {
                if let ty::Adt(adt, _) = tcx
                    .type_of(implementation)
                    .instantiate_identity()
                    .skip_normalization()
                    .kind()
                    && adt.did().krate == implementation.krate
                {
                    into.insert(adt.did());
                }
            }
            trait_def_id
        };
        let mut traced_types = FxHashSet::default();
        let mut cell_types = FxHashSet::default();
        let mut swept_types = FxHashSet::default();
        let mut holds_no_cells_types = FxHashSet::default();
        let mut keeps_alive_types = FxHashSet::default();
        let mut defers_collections_types = FxHashSet::default();
        let trace = collect("libjs_rust::gc::visitor::Trace", &mut traced_types);
        // NB: #[derive(Trace)] traces the fields, which the type shows. A hand-written trace() can visit cells that
        //     the type hides, as PropertyKey does in a tagged usize.
        let hand_traced_types = tcx
            .all_impls(trace)
            .filter(|&implementation| !tcx.def_span(implementation).from_expansion())
            .filter_map(|implementation| {
                match tcx
                    .type_of(implementation)
                    .instantiate_identity()
                    .skip_normalization()
                    .kind()
                {
                    ty::Adt(adt, _) if adt.did().krate == implementation.krate => Some(adt.did()),
                    _ => None,
                }
            })
            .collect();
        collect("libjs_rust::gc::class::GcCell", &mut cell_types);
        collect("libjs_rust::gc::weak::SweptAfterCollection", &mut swept_types);
        collect("libjs_rust::gc::lint_markers::HoldsNoCells", &mut holds_no_cells_types);
        collect("libjs_rust::gc::lint_markers::KeepsAlive", &mut keeps_alive_types);
        collect(
            "libjs_rust::gc::lint_markers::DefersCollections",
            &mut defers_collections_types,
        );
        self.traced_types = traced_types;
        self.cell_types = cell_types;
        self.swept_types = swept_types;
        self.holds_no_cells_types = holds_no_cells_types;
        self.keeps_alive_types = keeps_alive_types;
        self.defers_collections_types = defers_collections_types;
        self.hand_traced_types = hand_traced_types;
        self.trace_trait = Some(trace);
    }

    fn check_pat(&mut self, cx: &LateContext<'tcx>, pat: &'tcx Pat<'tcx>) {
        if !self.enabled {
            return;
        }
        let PatKind::Binding(_, hir_id, ident, _) = pat.kind else {
            return;
        };
        let what = match pat.span.desugaring_kind() {
            Some(DesugaringKind::ForLoop) => "the iterator".to_owned(),
            Some(_) => return,
            // Other lowerings (`?`) bind names without a span; the value reaches a checked local anyway.
            None if ident.span.is_dummy() => return,
            None => format!("`{ident}` of type"),
        };
        if self.in_defer_gc_scope(cx, hir_id) {
            return;
        }
        let results = cx.typeck_results();
        if results
            .pat_binding_modes()
            .get(hir_id)
            .is_some_and(|mode| matches!(mode.0, ByRef::Yes(..)))
        {
            return;
        }
        self.check_unscanned(cx, results.node_type(hir_id), hir_id, pat.span, &what);
    }

    fn check_fn(
        &mut self,
        cx: &LateContext<'tcx>,
        kind: FnKind<'tcx>,
        decl: &'tcx FnDecl<'tcx>,
        body: &'tcx Body<'tcx>,
        _: Span,
        def_id: LocalDefId,
    ) {
        if !self.enabled {
            return;
        }
        let output = match kind {
            FnKind::Closure => match cx
                .tcx
                .type_of(def_id)
                .instantiate_identity()
                .skip_normalization()
                .kind()
            {
                ty::Closure(_, args) => args.as_closure().sig().output().skip_binder(),
                _ => return,
            },
            FnKind::ItemFn(..) | FnKind::Method(..) => cx
                .tcx
                .fn_sig(def_id)
                .instantiate_identity()
                .skip_normalization()
                .output()
                .skip_binder(),
        };
        let span = match decl.output {
            FnRetTy::Return(ty) => ty.span,
            FnRetTy::DefaultReturn(_) => body.value.span,
        };
        // NB: What a function or closure that takes a deferral returns, like a cell that Heap::allocate() builds, is
        //     checked where its caller holds it, which the deferral outlives until a binding the checks see. What a
        //     closure in a deferral's scope returns stays in that scope, like a local.
        let hir_id = cx.tcx.local_def_id_to_hir_id(def_id);
        let takes_defer_gc = match kind {
            FnKind::Closure => {
                self.closure_takes_defer_gc(cx, def_id) || self.in_defer_gc_scope(cx, cx.tcx.parent_hir_id(hir_id))
            }
            FnKind::ItemFn(..) | FnKind::Method(..) => self.takes_defer_gc(cx, def_id),
        };
        if takes_defer_gc {
            return;
        }
        self.check_unscanned(cx, output, hir_id, span, "return type");
    }

    fn check_expr(&mut self, cx: &LateContext<'tcx>, expr: &'tcx Expr<'tcx>) {
        if !self.enabled {
            return;
        }
        // Boxing a closure into a `dyn Fn` hides its captures from the type of whatever holds it next, unless the trait
        // object still shows them by implementing Trace.
        let results = cx.typeck_results();
        let mut source = results.expr_ty(expr);
        for adjustment in results.expr_adjustments(expr) {
            if let Adjust::Pointer(PointerCoercion::Unsize) = adjustment.kind
                && !self.reaches_gc_pointer(cx, normalize(cx, adjustment.target), &mut Vec::new())
            {
                self.check_unscanned(cx, source, expr.hir_id, expr.span, "coercion from");
            }
            source = adjustment.target;
        }
    }

    fn check_item(&mut self, cx: &LateContext<'tcx>, item: &'tcx Item<'tcx>) {
        if !self.enabled
            || !matches!(
                item.kind,
                ItemKind::Struct(..) | ItemKind::Enum(..) | ItemKind::Union(..)
            )
        {
            return;
        }
        let def_id = item.owner_id.to_def_id();
        if !self.traced_types.contains(&def_id) {
            return;
        }
        let tcx = cx.tcx;
        let adt = tcx.adt_def(item.owner_id);
        for field in adt.all_fields() {
            let ty = tcx.type_of(field.did).instantiate_identity().skip_normalization();
            if let Some((root, _)) = self.find(cx, ty, Target::Root, &mut Vec::new(), None) {
                let field_id = field.did.expect_local();
                self.emit(
                    cx,
                    ROOT_IN_TRACED_TYPE,
                    tcx.local_def_id_to_hir_id(field_id),
                    tcx.def_span(field_id),
                    with_forced_trimmed_paths!(format!(
                        "'{}' implements Trace but holds the root '{root}'",
                        tcx.def_path_str(item.owner_id)
                    )),
                );
            }
        }
    }
}
