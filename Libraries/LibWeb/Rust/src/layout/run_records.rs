/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub(crate) struct RunRecords {
    root: Node,
    map: RefCell<HashMap<u32, std::rc::Rc<UsedValues>>>,
}

impl RunRecords {
    pub(crate) fn new(root: Node, root_used: std::rc::Rc<UsedValues>) -> Self {
        let records = Self::new_unrooted(root);
        records.register(root, root_used);
        records
    }

    pub(crate) fn new_unrooted(root: Node) -> Self {
        Self {
            root,
            map: RefCell::new(HashMap::new()),
        }
    }

    pub(crate) fn register(&self, node: Node, used: std::rc::Rc<UsedValues>) {
        let previous = self.map.borrow_mut().insert(node.slot_index(), used);
        assert!(
            previous.is_none(),
            "slot {} registered twice in the run rooted at slot {}",
            node.slot_index(),
            self.root.slot_index()
        );
    }

    pub(crate) fn create_used_values(
        &self,
        callbacks: &FfiLayoutFcCallbacks,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> std::rc::Rc<UsedValues> {
        let used = crate::layout::create_used_values(callbacks, node, constraints);
        self.register(node, used.clone());
        used
    }

    #[track_caller]
    pub(crate) fn used_values(&self, node: Node) -> std::rc::Rc<UsedValues> {
        let caller = std::panic::Location::caller();
        self.used_values_if_owned(node).unwrap_or_else(|| {
            panic!(
                "the run rooted at slot {} does not own the record for slot {} (read at {caller})",
                self.root.slot_index(),
                node.slot_index(),
            )
        })
    }

    pub(crate) fn used_values_if_owned(&self, node: Node) -> Option<std::rc::Rc<UsedValues>> {
        self.map.borrow().get(&node.slot_index()).cloned()
    }
}
