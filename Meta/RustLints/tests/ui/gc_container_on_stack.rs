/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mirrors Tests/ClangPlugins/LibJSGCTests/gc_pointer_container_on_stack.cpp.

#![allow(unused, clippy::all)]

use std::cell::RefCell;
use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::rc::Rc;
use std::sync::Arc;

use js::{Cell, Gc, GcWeak, MarkedVec, Root, Value, Vm};
use smallvec::SmallVec;

pub struct Entry {
    cell: Gc<Cell>,
}

pub struct EntryWithoutGcPointers {
    value: i32,
}

pub struct State {
    cells: Vec<Gc<Cell>>,
}

pub struct EntryWithArray {
    cells: [Option<Gc<Cell>>; 2],
}

pub struct Wrapper<T>(T);

pub enum Either {
    Cell(Gc<Cell>),
    Cells(Box<[Gc<Cell>]>),
}

pub fn make_cells() -> Vec<Gc<Cell>> { //~ ERROR return type 'Vec<Gc<Cell>>' keeps GC pointers in heap memory
    Vec::new()
}

pub fn make_value() -> Value {
    Value(0)
}

pub fn unrooted_locals(cell: Gc<Cell>) {
    let refs: Vec<Gc<Cell>> = Vec::new(); //~ ERROR `refs` of type 'Vec<Gc<Cell>>' keeps GC pointers in heap memory
    let values: Vec<Value> = Vec::new(); //~ ERROR 'Vec<Value>' keeps GC pointers
    let optional_refs: Vec<Option<Gc<Cell>>> = Vec::new(); //~ ERROR 'Vec<Option<Gc<Cell>>>'
    let nested: Vec<Vec<Gc<Cell>>> = Vec::new(); //~ ERROR 'Vec<Vec<Gc<Cell>>>'
    let entries: Vec<Entry> = Vec::new(); //~ ERROR 'Vec<Entry>'
    let map_values: HashMap<i32, Gc<Cell>> = HashMap::new(); //~ ERROR 'HashMap<i32, Gc<Cell>>'
    let map_keys: HashMap<Gc<Cell>, i32> = HashMap::new(); //~ ERROR 'HashMap<Gc<Cell>, i32>'
    let set: HashSet<Gc<Cell>> = HashSet::new(); //~ ERROR 'HashSet<Gc<Cell>>'
    let ordered: BTreeMap<i32, Value> = BTreeMap::new(); //~ ERROR 'BTreeMap<i32, Value>'
    let deque: VecDeque<Gc<Cell>> = VecDeque::new(); //~ ERROR 'VecDeque<Gc<Cell>>'
    let boxed = Box::new(cell); //~ ERROR 'Box<Gc<Cell>>'
    let shared = Rc::new(cell); //~ ERROR 'Rc<Gc<Cell>>'
    let atomic = Arc::new(make_value()); //~ ERROR 'Arc<Value>'
    let boxed_slice: Box<[Gc<Cell>]> = Box::new([cell]); //~ ERROR 'Box<[Gc<Cell>]>'
    let optional_vector: Option<Vec<Gc<Cell>>> = None; //~ ERROR keeps GC pointers in heap memory
    let state = State { cells: Vec::new() }; //~ ERROR `state` of type 'Vec<Gc<Cell>>'
    let entries_with_arrays: Vec<EntryWithArray> = Vec::new(); //~ ERROR 'Vec<EntryWithArray>'
    let groups: [Vec<Gc<Cell>>; 2] = [Vec::new(), Vec::new()]; //~ ERROR 'Vec<Gc<Cell>>'
    let wrapped: Wrapper<Vec<Value>> = Wrapper(Vec::new()); //~ ERROR 'Vec<Value>'
    let either = Either::Cell(cell); //~ ERROR 'Box<[Gc<Cell>]>'
    let cell_ref_cell: RefCell<Vec<Gc<Cell>>> = RefCell::new(Vec::new()); //~ ERROR 'Vec<Gc<Cell>>'
    let references: Vec<&Cell> = Vec::new(); //~ ERROR 'Vec<&Cell>'
    let deduced = make_cells(); //~ ERROR `deduced` of type 'Vec<Gc<Cell>>'
    let (first, second) = (make_cells(), 1); //~ ERROR `first` of type 'Vec<Gc<Cell>>'
    let State { cells } = State { cells: Vec::new() }; //~ ERROR `cells` of type 'Vec<Gc<Cell>>'
    let mut collected = Vec::new(); //~ ERROR `collected` of type 'Vec<Gc<Cell>>'
    collected.push(cell);
}

pub fn generic_local<T>(cell: Gc<T>) {
    let refs = vec![cell]; //~ ERROR 'Vec<Gc<T>>'
}

pub fn closure_local() {
    let closure = || {
        let refs: Vec<Gc<Cell>> = Vec::new(); //~ ERROR 'Vec<Gc<Cell>>'
    };
}

pub fn parameter_by_value(cells: Vec<Gc<Cell>>, state: &mut State) { //~ ERROR `cells` of type 'Vec<Gc<Cell>>'
    state.cells = cells;
}

pub fn state_by_value(state: State) {} //~ ERROR `state` of type 'Vec<Gc<Cell>>'

pub fn closure_parameter() {
    let closure = |cells: Vec<Gc<Cell>>| {}; //~ ERROR `cells` of type 'Vec<Gc<Cell>>'
}

pub struct Holder {
    cells: Vec<Gc<Cell>>,
}

impl Holder {
    pub fn new(cells: Vec<Gc<Cell>>) -> Self { //~ ERROR `cells` of type 'Vec<Gc<Cell>>'
        Self { cells } //~^ ERROR return type 'Vec<Gc<Cell>>'
    }

    pub fn cells(&self) -> &[Gc<Cell>] {
        &self.cells
    }

    pub fn cells_vec(&self) -> &Vec<Gc<Cell>> {
        &self.cells
    }

    pub fn copy_cells(&self) -> Vec<Gc<Cell>> { //~ ERROR return type 'Vec<Gc<Cell>>'
        self.cells.clone()
    }
}

pub trait Provider {
    fn declared_without_body(&self, cells: Vec<Gc<Cell>>);

    fn provided(&self) -> Vec<Value> { //~ ERROR return type 'Vec<Value>'
        Vec::new()
    }
}

pub fn rooted_or_borrowed_parameters(
    root: Root<'_, Vec<Gc<Cell>>>,
    marked: MarkedVec<'_, Gc<Cell>>,
    reference: &Vec<Gc<Cell>>,
    mutable_reference: &mut Vec<Gc<Cell>>,
    slice: &[Gc<Cell>],
) {
}

pub fn rooted_or_not_holding_gc_pointers(vm: &Vm, state: &State, cell: Gc<Cell>) {
    let marked = MarkedVec::<Gc<Cell>>::new(vm);
    let roots: Vec<Root<'_, Gc<Cell>>> = Vec::new();
    let weak_refs: Vec<GcWeak<Cell>> = Vec::new();
    let entries_without_gc_pointers: Vec<EntryWithoutGcPointers> = Vec::new();
    let integers: Vec<i32> = Vec::new();
    let member_reference: &Vec<Gc<Cell>> = &state.cells;
    let slice: &[Gc<Cell>] = &state.cells;
    let pointer_into_member: *const Gc<Cell> = state.cells.as_ptr();
    let single = cell;
    let optional = Some(cell);
    let array = [cell, cell];
    let tuple = (cell, make_value());
    let inline = Entry { cell };
    let cell_value = std::cell::Cell::new(cell);
    let iterator = state.cells.iter();
    for cell in &state.cells {}
    for cell in state.cells.iter().copied() {}
    if let Some(ref cells) = Some(&state.cells) {}
    match &Some(State { cells: Vec::new() }) {
        Some(State { cells }) => {}
        None => {}
    }
}

pub struct TracedStorage {
    cells: Vec<Gc<Cell>>,
}

unsafe impl js::Trace for TracedStorage {
    fn trace(&self, _: &mut js::Visitor) {}
}

impl TracedStorage {
    pub fn new(_: &js::DeferGc<'_>, cells: Vec<Gc<Cell>>) -> Self {
        Self { cells }
    }

    pub fn with_cell(mut self, cell: Gc<Cell>) -> Self { //~ ERROR `self` of type 'Vec<Gc<Cell>>' //~ ERROR return type 'Vec<Gc<Cell>>'
        self.cells.push(cell);
        self
    }
}

pub fn traced_storage_in_a_deferral(cell: Gc<Cell>) {
    let build = |defer_gc: &js::DeferGc<'_>| TracedStorage::new(defer_gc, vec![cell]);
}

pub fn traced_storage_outside_a_deferral(cell: Gc<Cell>, vm: &Vm) {
    let defer_gc = js::DeferGc(vm);
    let storage = TracedStorage::new(&defer_gc, Vec::new()); //~ ERROR `storage` of type 'Vec<Gc<Cell>>'
    let storages = vec![TracedStorage::new(&defer_gc, Vec::new())]; //~ ERROR 'Vec<TracedStorage>'
}

pub struct WeakCacheEntry {
    shape: Option<Gc<Cell>>,
}

unsafe impl js::gc::weak::SweptAfterCollection for WeakCacheEntry {}

pub fn swept_after_collection() -> Box<[WeakCacheEntry]> {
    let entries: Vec<WeakCacheEntry> = Vec::new();
    entries.into_boxed_slice()
}

pub fn traced_storage_in_a_box(storage: TracedStorage) -> Option<Box<TracedStorage>> { //~ ERROR `storage` of type 'Vec<Gc<Cell>>' //~ ERROR return type 'Box<TracedStorage>'
    let boxed = Box::new(storage); //~ ERROR `boxed` of type 'Box<TracedStorage>'
    Some(boxed)
}

pub fn takes_defer_gc(cell: Gc<Cell>, cells: Vec<Gc<Cell>>, _: &js::DeferGc<'_>) -> Vec<Gc<Cell>> {
    let local = vec![cell];
    let closure = || {
        let in_closure = vec![cell];
    };
    let returns_cells = || make_cells();
    cells
}

pub fn returns_deferred<'d>(cell: Gc<Cell>, defer_gc: &'d js::DeferGc<'_>) -> js::Deferred<'d, Vec<Gc<Cell>>> {
    defer_gc.cover(vec![cell])
}

pub fn closure_takes_defer_gc(cell: Gc<Cell>) {
    let build = |_: &js::DeferGc<'_>| {
        let in_closure = vec![cell];
    };
    let build_cells = |_: &js::DeferGc<'_>| make_cells();
    let nested = |_: &js::DeferGc<'_>| {
        let inner = || {
            let in_inner = vec![cell];
        };
    };
}

pub fn deferred_local(vm: &Vm, cell: Gc<Cell>) {
    let defer_gc = js::DeferGc(vm);
    let covered = defer_gc.cover(vec![cell]);
    let uncovered = covered.0; //~ ERROR `uncovered` of type 'Vec<Gc<Cell>>'
}

pub fn under_defer_gc(vm: &Vm, cell: Gc<Cell>) {
    let before = vec![cell]; //~ ERROR `before` of type 'Vec<Gc<Cell>>'
    let defer_gc = js::DeferGc(vm);
    let after = vec![cell]; //~ ERROR `after` of type 'Vec<Gc<Cell>>'
    drop(defer_gc);
    let closure = || {
        let in_closure = vec![cell]; //~ ERROR `in_closure` of type 'Vec<Gc<Cell>>'
    };
}

pub fn return_root(vm: &Vm) -> Root<'_, Vec<Gc<Cell>>> {
    Root {
        vm,
        value: Box::new(Vec::new()),
    }
}

pub fn return_reference(state: &State) -> &Vec<Gc<Cell>> {
    &state.cells
}

pub fn return_optional() -> Option<Vec<Gc<Cell>>> { //~ ERROR return type 'Vec<Gc<Cell>>'
    None
}

pub fn return_state() -> State { //~ ERROR return type 'Vec<Gc<Cell>>'
    State { cells: Vec::new() }
}

pub fn return_from_closure() {
    let closure = || make_cells(); //~ ERROR return type 'Vec<Gc<Cell>>'
}

pub fn range_over_temporary() {
    for cell in make_cells() {} //~ ERROR the iterator 'IntoIter<Gc<Cell>>'
}

pub fn question_mark() -> Result<(), ()> {
    let cells = Ok::<_, ()>(make_cells())?; //~ ERROR `cells` of type 'Vec<Gc<Cell>>'
    Ok(())
}

pub fn closures(cell: Gc<Cell>, state: &State) {
    let inline = move || cell;
    let borrowed = || state.cells.len();
    let boxed = Box::new(move || cell); //~ ERROR 'Box<{closure
    let job: Box<dyn Fn() -> Gc<Cell>> = Box::new(move || cell); //~ ERROR coercion from 'Box<{closure
    let borrowing_job: Box<dyn Fn() -> usize + '_> = Box::new(|| state.cells.len());
    let empty: Box<[Gc<Cell>]> = Box::new([]); //~ ERROR `empty` of type 'Box<[Gc<Cell>]>'
    run(Box::new(move || { //~ ERROR coercion from 'Box<{closure
        let captured = cell;
    }));
}

fn run(_: Box<dyn FnOnce()>) {}

pub fn small_vectors(cell: Gc<Cell>) {
    let spilled: SmallVec<[Gc<Cell>; 2]> = SmallVec::new(); //~ ERROR 'SmallVec<[Gc<Cell>; 2]>'
    let integers: SmallVec<[i32; 2]> = SmallVec::new();
}

pub fn suppressed(cell: Gc<Cell>) {
    #[cfg_attr(ladybird_gc_lints, expect(gc_container_on_stack, reason = "cell stays alive in the caller"))]
    let cells = vec![cell];

    #[cfg_attr(ladybird_gc_lints, allow(gc_container_on_stack))] //~ ERROR allowing `gc_container_on_stack` needs a `reason = "..."`
    let without_reason = vec![cell];

    #[cfg_attr(ladybird_gc_lints, expect(gc_container_on_stack, reason = "nothing to suppress"))] //~ WARN this lint expectation is unfulfilled
    let nothing = cell;
}

/// A cell pointer in a tagged word, which only its hand-written trace() knows about.
pub struct Tagged(usize);

unsafe impl js::Trace for Tagged {
    fn trace(&self, _: &mut js::Visitor) {}
}

pub trait TracedBehaviour: js::Trace {}

impl TracedBehaviour for Tagged {}

pub fn hand_traced(tagged: Tagged, more: Tagged) {
    let words = vec![tagged]; //~ ERROR `words` of type 'Vec<Tagged>'
    let behaviour: Box<dyn TracedBehaviour> = Box::new(more); //~ ERROR `behaviour` of type 'Box<dyn TracedBehaviour>'
}

/// A tagged word that holds no cell pointer, as a key that is never a symbol does.
pub struct Untagged(Tagged);

unsafe impl js::gc::lint_markers::HoldsNoCells for Untagged {}

pub fn holds_no_cells(untagged: Untagged) {
    let words = vec![untagged];
}
