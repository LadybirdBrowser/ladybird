/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A fake DOM host that defines the `ladybird_html_parser_*` callbacks so the tree builder can run in Rust tests.

use super::APPEND_CHILD_OFFSET;
use super::RustFfiHtmlNamespace;
use super::RustFfiHtmlParserAttribute;
use super::RustFfiHtmlQuirksMode;
use super::RustFfiHtmlShadowRootMode;
use super::RustFfiHtmlSlotAssignmentMode;
use std::cell::RefCell;
use std::ffi::c_void;
use std::ptr::NonNull;

pub(super) const DOCUMENT: usize = 1;

#[derive(Clone, Debug, Eq, PartialEq)]
pub(super) enum FakeNodeKind {
    Document,
    DocumentType,
    DocumentFragment,
    ShadowRoot,
    Element(RustFfiHtmlNamespace),
    Text(String),
    Comment,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum FakeInsertionCall {
    InsertNode,
    InsertText,
    ReinsertLastNode,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) struct FakeInsertion {
    pub(super) call: FakeInsertionCall,
    pub(super) parent: usize,
    pub(super) explicit_offset: Option<usize>,
}

struct FakeNode {
    kind: FakeNodeKind,
    parent: usize,
    children: Vec<usize>,
    fragment_host: usize,
    template_content: usize,
    shadow_root: usize,
}

struct FakeDom {
    nodes: Vec<FakeNode>,
    insertions: Vec<FakeInsertion>,
}

impl FakeDom {
    fn new() -> Self {
        let mut dom = Self {
            nodes: Vec::new(),
            insertions: Vec::new(),
        };
        let document = dom.create_node(FakeNodeKind::Document);
        assert_eq!(document, DOCUMENT);
        dom
    }

    fn create_node(&mut self, kind: FakeNodeKind) -> usize {
        self.nodes.push(FakeNode {
            kind,
            parent: 0,
            children: Vec::new(),
            fragment_host: 0,
            template_content: 0,
            shadow_root: 0,
        });
        self.nodes.len()
    }

    fn node(&self, handle: usize) -> &FakeNode {
        &self.nodes[handle - 1]
    }

    fn node_mut(&mut self, handle: usize) -> &mut FakeNode {
        &mut self.nodes[handle - 1]
    }

    fn index_in_parent(&self, node: usize) -> usize {
        let parent = self.node(node).parent;
        self.node(parent)
            .children
            .iter()
            .position(|&child| child == node)
            .unwrap()
    }

    fn has_element_child(&self, node: usize) -> bool {
        self.node(node)
            .children
            .iter()
            .any(|&child| matches!(self.node(child).kind, FakeNodeKind::Element(_)))
    }

    fn is_host_including_inclusive_ancestor(&self, ancestor: usize, node: usize) -> bool {
        let mut current = node;
        while current != 0 {
            if current == ancestor {
                return true;
            }
            let current_node = self.node(current);
            current = if current_node.parent != 0 {
                current_node.parent
            } else {
                current_node.fragment_host
            };
        }
        false
    }

    fn record_insertion(&mut self, call: FakeInsertionCall, parent: usize, offset: usize) {
        let explicit_offset = (offset != APPEND_CHILD_OFFSET).then_some(offset);
        if let Some(offset) = explicit_offset {
            assert!(offset <= self.node(parent).children.len());
        }
        self.insertions.push(FakeInsertion {
            call,
            parent,
            explicit_offset,
        });
    }

    fn child_at_offset(&self, parent: usize, offset: usize) -> Option<usize> {
        if offset == APPEND_CHILD_OFFSET {
            return None;
        }
        self.node(parent).children.get(offset).copied()
    }

    fn child_before_offset(&self, parent: usize, offset: usize) -> Option<usize> {
        let children = &self.node(parent).children;
        match offset {
            APPEND_CHILD_OFFSET => children.last().copied(),
            0 => None,
            _ => Some(children[offset - 1]),
        }
    }

    fn remove(&mut self, node: usize) {
        let parent = self.node(node).parent;
        if parent == 0 {
            return;
        }
        let index = self.index_in_parent(node);
        self.node_mut(parent).children.remove(index);
        self.node_mut(node).parent = 0;
    }

    fn insert_before(&mut self, parent: usize, node: usize, reference_child: Option<usize>) {
        self.remove(node);
        let index = match reference_child {
            Some(reference_child) => self.index_in_parent(reference_child),
            None => self.node(parent).children.len(),
        };
        self.node_mut(parent).children.insert(index, node);
        self.node_mut(node).parent = parent;
    }
}

thread_local! {
    static FAKE_DOM: RefCell<FakeDom> = RefCell::new(FakeDom::new());
}

fn with_fake_dom<R>(callback: impl FnOnce(&mut FakeDom) -> R) -> R {
    FAKE_DOM.with_borrow_mut(callback)
}

pub(super) fn fake_parser_host() -> *mut c_void {
    NonNull::<c_void>::dangling().as_ptr()
}

pub(super) fn reset() {
    with_fake_dom(|dom| *dom = FakeDom::new());
}

pub(super) fn insertions() -> Vec<FakeInsertion> {
    with_fake_dom(|dom| dom.insertions.clone())
}

pub(super) fn children(node: usize) -> Vec<usize> {
    with_fake_dom(|dom| dom.node(node).children.clone())
}

pub(super) fn kind(node: usize) -> FakeNodeKind {
    with_fake_dom(|dom| dom.node(node).kind.clone())
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_document_node(_parser: *mut c_void) -> usize {
    DOCUMENT
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_document_html_element(_parser: *mut c_void) -> usize {
    with_fake_dom(|dom| {
        let document_element = dom
            .node(DOCUMENT)
            .children
            .iter()
            .copied()
            .find(|&child| matches!(dom.node(child).kind, FakeNodeKind::Element(_)));
        match document_element {
            Some(element) if dom.node(element).kind == FakeNodeKind::Element(RustFfiHtmlNamespace::Html) => element,
            _ => 0,
        }
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_set_document_quirks_mode(_parser: *mut c_void, _mode: RustFfiHtmlQuirksMode) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_document_is_iframe_srcdoc_document(_parser: *mut c_void) -> bool {
    false
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_log_parse_error(_parser: *mut c_void, _message_ptr: *const u8, _message_len: usize) {
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_stop_parsing(_parser: *mut c_void) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_parse_errors_enabled() -> bool {
    false
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_visit_node(_visitor: *mut c_void, _node: usize) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_create_document_type(
    _parser: *mut c_void,
    _name_ptr: *const u16,
    _name_len: usize,
    _public_id_ptr: *const u16,
    _public_id_len: usize,
    _system_id_ptr: *const u16,
    _system_id_len: usize,
) -> usize {
    with_fake_dom(|dom| dom.create_node(FakeNodeKind::DocumentType))
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_create_comment(
    _parser: *mut c_void,
    _data_ptr: *const u16,
    _data_len: usize,
) -> usize {
    with_fake_dom(|dom| dom.create_node(FakeNodeKind::Comment))
}

#[unsafe(no_mangle)]
unsafe extern "C" fn ladybird_html_parser_insert_text(
    parent: usize,
    offset: usize,
    data_ptr: *const u8,
    data_len: usize,
) {
    let data = std::str::from_utf8(unsafe { ak::slice_from_raw_parts(data_ptr, data_len) }).unwrap();
    with_fake_dom(|dom| {
        dom.record_insertion(FakeInsertionCall::InsertText, parent, offset);
        if dom.node(parent).kind == FakeNodeKind::Document {
            return;
        }
        if let Some(previous_child) = dom.child_before_offset(parent, offset)
            && let FakeNodeKind::Text(previous_text) = &mut dom.node_mut(previous_child).kind
        {
            previous_text.push_str(data);
            return;
        }
        let reference_child = dom.child_at_offset(parent, offset);
        let text = dom.create_node(FakeNodeKind::Text(data.to_string()));
        dom.insert_before(parent, text, reference_child);
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_add_missing_attribute(
    _parser: *mut c_void,
    _element: usize,
    _local_name: usize,
    _value_ptr: *const u16,
    _value_len: usize,
) {
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_remove_node(node: usize) {
    with_fake_dom(|dom| dom.remove(node))
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_handle_element_popped(_element: usize) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_set_script_source_line(
    _parser: *mut c_void,
    _element: usize,
    _source_line_number: usize,
) {
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_mark_script_already_started(_parser: *mut c_void, _element: usize) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_process_meta_element(_parser: *mut c_void, _element: usize) {}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_parent_node(node: usize) -> usize {
    with_fake_dom(|dom| dom.node(node).parent)
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_element_cannot_be_inserted(target_parent: usize, element: usize) -> bool {
    with_fake_dom(|dom| {
        dom.node(element).parent != 0
            || dom.is_host_including_inclusive_ancestor(element, target_parent)
            || (dom.node(target_parent).kind == FakeNodeKind::Document && dom.has_element_child(target_parent))
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_node_index(node: usize) -> usize {
    with_fake_dom(|dom| dom.index_in_parent(node))
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_create_element(
    _parser: *mut c_void,
    _intended_parent: usize,
    namespace_: RustFfiHtmlNamespace,
    _namespace_uri_ptr: *const u16,
    _namespace_uri_len: usize,
    _local_name: usize,
    _attributes: *const RustFfiHtmlParserAttribute,
    _attribute_count: usize,
    _had_duplicate_attribute: bool,
    _form_element: usize,
    _has_template_element_on_stack: bool,
) -> usize {
    with_fake_dom(|dom| dom.create_node(FakeNodeKind::Element(namespace_)))
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_append_child(parent: usize, child: usize) {
    with_fake_dom(|dom| dom.insert_before(parent, child, None))
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_insert_node(
    parent: usize,
    offset: usize,
    child: usize,
    _queue_custom_element_reactions: bool,
) {
    with_fake_dom(|dom| {
        dom.record_insertion(FakeInsertionCall::InsertNode, parent, offset);
        let reference_child = dom.child_at_offset(parent, offset);
        dom.insert_before(parent, child, reference_child);
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_reinsert_last_node(parent: usize, offset: usize, last_node: usize) {
    with_fake_dom(|dom| {
        dom.record_insertion(FakeInsertionCall::ReinsertLastNode, parent, offset);
        let reference_child = dom.child_at_offset(parent, offset);
        dom.remove(last_node);
        let can_be_inserted = !dom.is_host_including_inclusive_ancestor(last_node, parent)
            && (dom.node(parent).kind != FakeNodeKind::Document || !dom.has_element_child(parent))
            && reference_child.is_none_or(|reference_child| dom.node(reference_child).parent == parent);
        if can_be_inserted {
            dom.insert_before(parent, last_node, reference_child);
        }
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_move_all_children(from: usize, to: usize) {
    with_fake_dom(|dom| {
        for child in dom.node(from).children.clone() {
            dom.insert_before(to, child, None);
        }
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_template_content(element: usize) -> usize {
    with_fake_dom(|dom| {
        if dom.node(element).template_content == 0 {
            let content = dom.create_node(FakeNodeKind::DocumentFragment);
            dom.node_mut(content).fragment_host = element;
            dom.node_mut(element).template_content = content;
        }
        dom.node(element).template_content
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_attach_declarative_shadow_root(
    host: usize,
    _mode: RustFfiHtmlShadowRootMode,
    _slot_assignment: RustFfiHtmlSlotAssignmentMode,
    _clonable: bool,
    _serializable: bool,
    _delegates_focus: bool,
    _keep_custom_element_registry_null: bool,
) -> usize {
    with_fake_dom(|dom| {
        if dom.node(host).shadow_root != 0 {
            return 0;
        }
        let shadow_root = dom.create_node(FakeNodeKind::ShadowRoot);
        dom.node_mut(shadow_root).fragment_host = host;
        dom.node_mut(host).shadow_root = shadow_root;
        shadow_root
    })
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_set_template_content(element: usize, content: usize) {
    with_fake_dom(|dom| dom.node_mut(element).template_content = content)
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_is_shadow_host(node: usize) -> bool {
    with_fake_dom(|dom| dom.node(node).shadow_root != 0)
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_sanitizer_action(_parser: *mut c_void, _element: usize) -> u8 {
    0
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_child_at_index(parent: usize, index: usize) -> usize {
    with_fake_dom(|dom| dom.node(parent).children.get(index).copied().unwrap_or(0))
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_declarative_shadow_root_attributes(_element: usize) -> u32 {
    0
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_html_parser_set_script_text_from_parser(_element: usize) {}
