/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::RustFfiTokenizerHandle;
use crate::interned_names;
use crate::token::{Token, TokenPayload, TokenType};
use crate::tokenizer::{HtmlTokenizer, State};
use std::ffi::c_void;

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlParserRunResult {
    Ok = 0,
    Unsupported = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlNamespace {
    Html = 0,
    MathMl = 1,
    Svg = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlQuirksMode {
    No = 0,
    Limited = 1,
    Yes = 2,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlShadowRootMode {
    Open = 0,
    Closed = 1,
}

#[repr(C)]
pub struct RustFfiHtmlParserAttribute {
    pub local_name_ptr: *const u8,
    pub local_name_len: usize,
    pub value_ptr: *const u8,
    pub value_len: usize,
}

unsafe extern "C" {
    fn ladybird_html_parser_document_node(parser: *mut c_void) -> usize;
    fn ladybird_html_parser_set_document_quirks_mode(parser: *mut c_void, mode: RustFfiHtmlQuirksMode);
    fn ladybird_html_parser_create_document_type(
        parser: *mut c_void,
        name_ptr: *const u8,
        name_len: usize,
        public_id_ptr: *const u8,
        public_id_len: usize,
        system_id_ptr: *const u8,
        system_id_len: usize,
    ) -> usize;
    fn ladybird_html_parser_create_comment(parser: *mut c_void, data_ptr: *const u8, data_len: usize) -> usize;
    fn ladybird_html_parser_create_text_node(parser: *mut c_void, data_ptr: *const u8, data_len: usize) -> usize;
    fn ladybird_html_parser_create_element(
        parser: *mut c_void,
        namespace_: RustFfiHtmlNamespace,
        local_name_ptr: *const u8,
        local_name_len: usize,
        attributes: *const RustFfiHtmlParserAttribute,
        attribute_count: usize,
    ) -> usize;
    fn ladybird_html_parser_append_child(parent: usize, child: usize);
    fn ladybird_html_parser_insert_before(parent: usize, child: usize, before: usize);
    fn ladybird_html_parser_template_content(element: usize) -> usize;
    fn ladybird_html_parser_attach_declarative_shadow_root(
        host: usize,
        mode: RustFfiHtmlShadowRootMode,
        clonable: bool,
        serializable: bool,
        delegates_focus: bool,
        keep_custom_element_registry_null: bool,
    ) -> usize;
}

/// Opaque handle for the Rust HTML parser, passed across the FFI boundary.
pub struct RustFfiHtmlParserHandle {
    run_count: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum InsertionMode {
    Initial,
    BeforeHtml,
    BeforeHead,
    InHead,
    InHeadNoscript,
    AfterHead,
    InBody,
    Text,
    InTable,
    InCaption,
    InTableBody,
    InRow,
    InCell,
    InFrameset,
    AfterFrameset,
    AfterBody,
}

#[derive(Clone, Debug)]
struct OwnedAttribute {
    local_name: String,
    value: String,
}

#[derive(Clone, Debug)]
struct StackNode {
    handle: usize,
    local_name: String,
    namespace_: RustFfiHtmlNamespace,
    attributes: Vec<OwnedAttribute>,
    template_content: Option<usize>,
}

#[derive(Clone, Debug)]
struct ActiveFormattingElement {
    handle: usize,
    local_name: String,
    attributes: Vec<OwnedAttribute>,
}

struct TreeBuilder<'a> {
    tokenizer: &'a mut HtmlTokenizer,
    host: *mut c_void,
    stack_of_open_elements: Vec<StackNode>,
    list_of_active_formatting_elements: Vec<ActiveFormattingElement>,
    insertion_mode: InsertionMode,
    original_insertion_mode: InsertionMode,
    head_element: Option<usize>,
    form_element: Option<usize>,
    next_line_feed_can_be_ignored: bool,
    pending_text: String,
}

impl<'a> TreeBuilder<'a> {
    fn new(tokenizer: &'a mut HtmlTokenizer, host: *mut c_void) -> Self {
        Self {
            tokenizer,
            host,
            stack_of_open_elements: Vec::new(),
            list_of_active_formatting_elements: Vec::new(),
            insertion_mode: InsertionMode::Initial,
            original_insertion_mode: InsertionMode::Initial,
            head_element: None,
            form_element: None,
            next_line_feed_can_be_ignored: false,
            pending_text: String::new(),
        }
    }

    fn run(&mut self, stop_at_insertion_point: bool) {
        loop {
            let cdata_allowed = self
                .adjusted_current_node()
                .is_some_and(|node| node.namespace_ != RustFfiHtmlNamespace::Html);
            let Some(token) = self.tokenizer.next_token(stop_at_insertion_point, cdata_allowed) else {
                break;
            };
            if self.next_line_feed_can_be_ignored {
                self.next_line_feed_can_be_ignored = false;
                if token.token_type == TokenType::Character && token.code_point == '\n' as u32 {
                    continue;
                }
            }
            let is_character = token.token_type == TokenType::Character;
            if !is_character {
                self.flush_character_insertions();
            }
            let is_eof = token.token_type == TokenType::EndOfFile;
            if self.should_process_token_using_html_rules(&token) {
                self.process_using_the_rules_for(self.insertion_mode, token);
            } else {
                self.process_using_the_rules_for_foreign_content(token);
            }
            if is_eof {
                break;
            }
        }
        self.flush_character_insertions();
    }

    fn process_using_the_rules_for(&mut self, mode: InsertionMode, token: Token) {
        match mode {
            InsertionMode::Initial => self.handle_initial(token),
            InsertionMode::BeforeHtml => self.handle_before_html(token),
            InsertionMode::BeforeHead => self.handle_before_head(token),
            InsertionMode::InHead => self.handle_in_head(token),
            InsertionMode::InHeadNoscript => self.handle_in_head_noscript(token),
            InsertionMode::AfterHead => self.handle_after_head(token),
            InsertionMode::InBody => self.handle_in_body(token),
            InsertionMode::Text => self.handle_text(token),
            InsertionMode::InTable => self.handle_in_table(token),
            InsertionMode::InCaption => self.handle_in_caption(token),
            InsertionMode::InTableBody => self.handle_in_table_body(token),
            InsertionMode::InRow => self.handle_in_row(token),
            InsertionMode::InCell => self.handle_in_cell(token),
            InsertionMode::InFrameset => self.handle_in_frameset(token),
            InsertionMode::AfterFrameset => self.handle_after_frameset(token),
            InsertionMode::AfterBody => self.handle_after_body(token),
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#tree-construction-dispatcher
    fn should_process_token_using_html_rules(&self, token: &Token) -> bool {
        self.stack_of_open_elements.is_empty()
            || self
                .adjusted_current_node()
                .is_some_and(|node| node.namespace_ == RustFfiHtmlNamespace::Html)
            || token.token_type == TokenType::EndOfFile
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-initial-insertion-mode
    fn handle_initial(&mut self, token: Token) {
        // -> A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF),
        //    U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), or U+0020 SPACE
        if token.is_parser_whitespace() {
            return;
        }

        // -> A comment token
        if token.token_type == TokenType::Comment {
            let document = self.document_node();
            self.append_comment_to_node(document, token.comment_data());
            return;
        }

        // -> A DOCTYPE token
        if token.token_type == TokenType::Doctype {
            let doctype = token.doctype_data();
            let name = if doctype.missing_name {
                ""
            } else {
                doctype.name.as_str()
            };
            let public_id = if doctype.missing_public_identifier {
                ""
            } else {
                doctype.public_identifier.as_str()
            };
            let system_id = if doctype.missing_system_identifier {
                ""
            } else {
                doctype.system_identifier.as_str()
            };
            let document_type = self.create_document_type(name, public_id, system_id);
            let document = self.document_node();
            self.append_child(document, document_type);
            self.set_document_quirks_mode(self.which_quirks_mode(&token));
            self.insertion_mode = InsertionMode::BeforeHtml;
            return;
        }

        // -> Anything else
        self.set_document_quirks_mode(RustFfiHtmlQuirksMode::Yes);
        self.insertion_mode = InsertionMode::BeforeHtml;
        self.process_using_the_rules_for(InsertionMode::BeforeHtml, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-before-html-insertion-mode
    fn handle_before_html(&mut self, token: Token) {
        if token.token_type == TokenType::Doctype || token.is_parser_whitespace() {
            return;
        }

        if token.token_type == TokenType::Comment {
            let document = self.document_node();
            self.append_comment_to_node(document, token.comment_data());
            return;
        }

        if token.is_start_tag_named("html") {
            let document = self.document_node();
            self.insert_html_element_for(&token, document);
            self.insertion_mode = InsertionMode::BeforeHead;
            return;
        }

        if token.is_end_tag_one_of(&["head", "body", "html", "br"]) || !token.is_end_tag() {
            let document = self.document_node();
            self.insert_html_element_named("html", document);
            self.insertion_mode = InsertionMode::BeforeHead;
            self.process_using_the_rules_for(InsertionMode::BeforeHead, token);
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-before-head-insertion-mode
    fn handle_before_head(&mut self, token: Token) {
        if token.is_parser_whitespace() || token.token_type == TokenType::Doctype {
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_named("head") {
            let head = self.insert_html_element_for(&token, self.current_node_handle());
            self.head_element = Some(head);
            self.insertion_mode = InsertionMode::InHead;
            return;
        }

        if token.is_end_tag_one_of(&["head", "body", "html", "br"]) || !token.is_end_tag() {
            let head = self.insert_html_element_named("head", self.current_node_handle());
            self.head_element = Some(head);
            self.insertion_mode = InsertionMode::InHead;
            self.process_using_the_rules_for(InsertionMode::InHead, token);
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead
    fn handle_in_head(&mut self, token: Token) {
        if token.is_parser_whitespace() {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_one_of(&["base", "basefont", "bgsound", "link", "meta"]) {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_start_tag_named("title") {
            self.parse_generic_rcdata_element(token);
            return;
        }

        if token.is_start_tag_one_of(&["style", "noframes"]) {
            self.parse_generic_raw_text_element(token);
            return;
        }

        if token.is_start_tag_named("noscript") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InHeadNoscript;
            return;
        }

        if token.is_start_tag_named("template") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InBody;
            return;
        }

        if token.is_start_tag_named("script") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.tokenizer.switch_to(State::ScriptData);
            self.original_insertion_mode = self.insertion_mode;
            self.insertion_mode = InsertionMode::Text;
            return;
        }

        if token.is_end_tag_named("head") {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::AfterHead;
            return;
        }

        if token.is_end_tag_one_of(&["body", "html", "br"]) || !token.is_end_tag() {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::AfterHead;
            self.process_using_the_rules_for(InsertionMode::AfterHead, token);
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inheadnoscript
    fn handle_in_head_noscript(&mut self, token: Token) {
        if token.is_end_tag_named("noscript") {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InHead;
            return;
        }

        if token.is_parser_whitespace() || token.token_type == TokenType::Comment {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        self.pop_current_node();
        self.insertion_mode = InsertionMode::InHead;
        self.process_using_the_rules_for(InsertionMode::InHead, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-after-head-insertion-mode
    fn handle_after_head(&mut self, token: Token) {
        if token.is_parser_whitespace() {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_named("body") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InBody;
            return;
        }

        if token.is_start_tag_named("frameset") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InFrameset;
            return;
        }

        if token.is_end_tag_one_of(&["body", "html", "br"]) || !token.is_end_tag() {
            self.insert_html_element_named("body", self.current_node_handle());
            self.insertion_mode = InsertionMode::InBody;
            self.process_using_the_rules_for(InsertionMode::InBody, token);
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody
    fn handle_in_body(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            if token.code_point == 0 {
                return;
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            return;
        }

        if token.is_start_tag_named("html") || token.is_start_tag_named("body") {
            return;
        }

        if token.is_start_tag_one_of(&["title"]) {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_named("template") {
            if let Some(shadow_root) = self.try_to_start_declarative_shadow_root(&token) {
                self.stack_of_open_elements.push(StackNode {
                    handle: shadow_root,
                    local_name: "template".to_string(),
                    namespace_: RustFfiHtmlNamespace::Html,
                    attributes: owned_attributes_from_token(&token, RustFfiHtmlNamespace::Html),
                    template_content: Some(shadow_root),
                });
                return;
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("p") {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("li") {
            if self.has_in_list_item_scope("li") {
                self.pop_until_tag_name_has_been_popped("li");
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_one_of(&["dd", "dt"]) {
            if self.has_in_scope("dd") {
                self.pop_until_tag_name_has_been_popped("dd");
            }
            if self.has_in_scope("dt") {
                self.pop_until_tag_name_has_been_popped("dt");
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("button") {
            if self.has_in_scope("button") {
                self.pop_until_tag_name_has_been_popped("button");
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("form") {
            if self.form_element.is_some() {
                return;
            }
            let form = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.form_element = Some(form);
            return;
        }

        if token.is_end_tag_named("form") {
            self.form_element = None;
            self.pop_until_tag_name_has_been_popped("form");
            return;
        }

        if token.is_start_tag_one_of(&["h1", "h2", "h3", "h4", "h5", "h6"]) {
            if self.current_node_is_heading() {
                self.pop_current_node();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_one_of(&["pre", "listing"]) {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.next_line_feed_can_be_ignored = true;
            return;
        }

        if token.is_start_tag_named("plaintext") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.tokenizer.switch_to(State::PLAINTEXT);
            return;
        }

        if token.is_start_tag_named("image") {
            self.insert_html_element_for_with_name(&token, "img", self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_start_tag_named("table") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_end_tag_named("template") {
            self.pop_until_tag_name_has_been_popped("template");
            if self.current_node_named("head") {
                self.insertion_mode = InsertionMode::InHead;
            }
            return;
        }

        if token.is_start_tag_named("svg") {
            self.insert_foreign_element_for(&token, RustFfiHtmlNamespace::Svg);
            return;
        }

        if token.is_start_tag_named("math") {
            self.insert_foreign_element_for(&token, RustFfiHtmlNamespace::MathMl);
            return;
        }

        if token.is_start_tag_named("option") {
            if self.current_node_named("option") {
                self.pop_current_node();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("optgroup") {
            if self.current_node_named("option") {
                self.pop_current_node();
            }
            if self.current_node_named("optgroup") {
                self.pop_current_node();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_end_tag_named("option") {
            if self.current_node_named("option") {
                self.pop_current_node();
            }
            return;
        }

        if token.is_end_tag_named("optgroup") {
            if self.current_node_named("option") {
                self.pop_current_node();
            }
            if self.current_node_named("optgroup") {
                self.pop_current_node();
            }
            return;
        }

        if token.is_end_tag_named("select") {
            self.pop_until_tag_name_has_been_popped("select");
            return;
        }

        if token.is_start_tag_one_of(&["textarea"]) {
            self.parse_generic_rcdata_element(token);
            return;
        }

        if token.is_start_tag_one_of(&["style", "xmp", "iframe", "noembed", "noframes"]) {
            self.parse_generic_raw_text_element(token);
            return;
        }

        if token.is_start_tag_named("br") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_start_tag() && is_formatting_element(token.tag_name()) {
            self.reconstruct_the_active_formatting_elements();
            let element = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.list_of_active_formatting_elements.push(ActiveFormattingElement {
                handle: element,
                local_name: token.tag_name().to_string(),
                attributes: owned_attributes_from_token(&token, RustFfiHtmlNamespace::Html),
            });
            return;
        }

        if token.is_end_tag() && is_formatting_element(token.tag_name()) {
            self.run_the_adoption_agency_algorithm(token.tag_name());
            return;
        }

        if token.is_start_tag() {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            if token.is_self_closing() || is_void_html_element(token.tag_name()) {
                self.pop_current_node();
            }
            return;
        }

        if token.is_end_tag_named("body") {
            self.insertion_mode = InsertionMode::AfterBody;
            return;
        }

        if token.is_end_tag_named("html") {
            self.insertion_mode = InsertionMode::AfterBody;
            self.process_using_the_rules_for(InsertionMode::AfterBody, token);
            return;
        }

        if token.is_end_tag() {
            self.pop_until_tag_name_has_been_popped(token.tag_name());
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inframeset
    fn handle_in_frameset(&mut self, token: Token) {
        if token.is_parser_whitespace() {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_named("frameset") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_end_tag_named("frameset") {
            self.pop_current_node();
            if !self.current_node_named("frameset") {
                self.insertion_mode = InsertionMode::AfterFrameset;
            }
            return;
        }

        if token.is_start_tag_named("frame") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-afterframeset
    fn handle_after_frameset(&mut self, token: Token) {
        if token.is_parser_whitespace() {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intable
    fn handle_in_table(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            if token.is_parser_whitespace() {
                self.insert_character(token.code_point);
            } else {
                self.foster_parent_character(token.code_point);
            }
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            return;
        }

        if token.is_start_tag_one_of(&["tbody", "tfoot", "thead"]) {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTableBody;
            return;
        }

        if token.is_start_tag_named("caption") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InCaption;
            return;
        }

        if token.is_start_tag_named("tr") {
            self.insert_html_element_named("tbody", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTableBody;
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_start_tag_one_of(&["td", "th"]) {
            self.insert_html_element_named("tbody", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTableBody;
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_end_tag_named("table") {
            self.pop_until_tag_name_has_been_popped("table");
            self.insertion_mode = InsertionMode::InBody;
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-incaption
    fn handle_in_caption(&mut self, token: Token) {
        if token.is_end_tag_named("caption") {
            self.pop_until_tag_name_has_been_popped("caption");
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_start_tag_one_of(&[
            "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) || token.is_end_tag_named("table")
        {
            self.pop_until_tag_name_has_been_popped("caption");
            self.insertion_mode = InsertionMode::InTable;
            self.process_using_the_rules_for(InsertionMode::InTable, token);
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intbody
    fn handle_in_table_body(&mut self, token: Token) {
        if token.is_start_tag_one_of(&["tbody", "tfoot", "thead"]) {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InTable;
            self.process_using_the_rules_for(InsertionMode::InTable, token);
            return;
        }

        if token.is_start_tag_named("tr") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InRow;
            return;
        }

        if token.is_start_tag_one_of(&["td", "th"]) {
            self.insert_html_element_named("tr", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InRow;
            self.process_using_the_rules_for(InsertionMode::InRow, token);
            return;
        }

        if token.is_end_tag_one_of(&["tbody", "tfoot", "thead"]) {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_end_tag_named("table") {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InTable;
            self.process_using_the_rules_for(InsertionMode::InTable, token);
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InTable, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intr
    fn handle_in_row(&mut self, token: Token) {
        if token.is_start_tag_one_of(&["td", "th"]) {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InCell;
            return;
        }

        if token.is_end_tag_named("tr") {
            self.pop_until_tag_name_has_been_popped("tr");
            self.insertion_mode = InsertionMode::InTableBody;
            return;
        }

        if token.is_start_tag_one_of(&["tbody", "tfoot", "thead"]) {
            self.process_using_the_rules_for(InsertionMode::InRow, Token::synthetic_end_tag("tr"));
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_end_tag_named("table") {
            self.process_using_the_rules_for(InsertionMode::InRow, Token::synthetic_end_tag("tr"));
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InTable, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intd
    fn handle_in_cell(&mut self, token: Token) {
        if token.is_end_tag_one_of(&["td", "th"]) {
            self.pop_until_one_of_tag_names_has_been_popped(&["td", "th"]);
            self.insertion_mode = InsertionMode::InRow;
            return;
        }

        if token.is_start_tag_one_of(&[
            "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) || token.is_end_tag_one_of(&["table", "tbody", "tfoot", "thead", "tr"])
        {
            self.pop_until_one_of_tag_names_has_been_popped(&["td", "th"]);
            self.insertion_mode = InsertionMode::InRow;
            self.process_using_the_rules_for(InsertionMode::InRow, token);
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-incdata
    fn handle_text(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            self.pop_current_node();
            self.insertion_mode = self.original_insertion_mode;
            self.process_using_the_rules_for(self.insertion_mode, token);
            return;
        }

        if token.is_end_tag() {
            self.pop_current_node();
            self.insertion_mode = self.original_insertion_mode;
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-after-body-insertion-mode
    fn handle_after_body(&mut self, token: Token) {
        if token.is_parser_whitespace() {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.token_type == TokenType::Comment {
            if let Some(html) = self.stack_of_open_elements.first() {
                self.append_comment_to_node(html.handle, token.comment_data());
            }
            return;
        }

        if token.is_end_tag_named("html") || token.token_type == TokenType::EndOfFile {
            return;
        }

        self.insertion_mode = InsertionMode::InBody;
        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign
    fn process_using_the_rules_for_foreign_content(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.is_start_tag() {
            let namespace_ = self
                .adjusted_current_node()
                .map(|node| node.namespace_)
                .unwrap_or(RustFfiHtmlNamespace::Html);
            self.insert_foreign_element_for(&token, namespace_);
            if token.is_self_closing() {
                self.pop_current_node();
            }
            return;
        }

        if token.is_end_tag() {
            self.pop_until_tag_name_has_been_popped(adjusted_foreign_end_tag_name(token.tag_name()));
        }
    }

    fn parse_generic_rcdata_element(&mut self, token: Token) {
        self.insert_html_element_for(&token, self.current_insertion_parent_handle());
        self.tokenizer.switch_to(State::RCDATA);
        self.original_insertion_mode = self.insertion_mode;
        self.insertion_mode = InsertionMode::Text;
    }

    fn parse_generic_raw_text_element(&mut self, token: Token) {
        self.insert_html_element_for(&token, self.current_insertion_parent_handle());
        self.tokenizer.switch_to(State::RAWTEXT);
        self.original_insertion_mode = self.insertion_mode;
        self.insertion_mode = InsertionMode::Text;
    }

    fn which_quirks_mode(&self, token: &Token) -> RustFfiHtmlQuirksMode {
        let doctype = token.doctype_data();
        if doctype.force_quirks {
            return RustFfiHtmlQuirksMode::Yes;
        }
        if doctype.name != "html" {
            return RustFfiHtmlQuirksMode::Yes;
        }
        RustFfiHtmlQuirksMode::No
    }

    fn document_node(&mut self) -> usize {
        unsafe { ladybird_html_parser_document_node(self.host) }
    }

    fn current_node_handle(&self) -> usize {
        self.stack_of_open_elements.last().map(|node| node.handle).unwrap_or(0)
    }

    fn current_insertion_parent_handle(&self) -> usize {
        self.stack_of_open_elements
            .last()
            .and_then(|node| node.template_content.or(Some(node.handle)))
            .unwrap_or(0)
    }

    fn current_node_named(&self, tag_name: &str) -> bool {
        self.stack_of_open_elements
            .last()
            .is_some_and(|node| node.local_name == tag_name && node.namespace_ == RustFfiHtmlNamespace::Html)
    }

    fn adjusted_current_node(&self) -> Option<&StackNode> {
        self.stack_of_open_elements.last()
    }

    fn insert_html_element_named(&mut self, name: &str, parent: usize) -> usize {
        self.flush_character_insertions();
        let element = self.create_element(RustFfiHtmlNamespace::Html, name, &[]);
        self.append_child(parent, element);
        let template_content = if name == "template" {
            Some(self.template_content(element))
        } else {
            None
        };
        self.stack_of_open_elements.push(StackNode {
            handle: element,
            local_name: name.to_string(),
            namespace_: RustFfiHtmlNamespace::Html,
            attributes: Vec::new(),
            template_content,
        });
        element
    }

    fn insert_html_element_for(&mut self, token: &Token, parent: usize) -> usize {
        self.insert_element_for(token, RustFfiHtmlNamespace::Html, parent)
    }

    fn insert_html_element_for_with_name(&mut self, token: &Token, local_name: &str, parent: usize) -> usize {
        self.insert_element_for_with_name(token, RustFfiHtmlNamespace::Html, local_name, parent)
    }

    fn insert_foreign_element_for(&mut self, token: &Token, namespace_: RustFfiHtmlNamespace) -> usize {
        self.insert_element_for(token, namespace_, self.current_insertion_parent_handle())
    }

    fn insert_element_for(&mut self, token: &Token, namespace_: RustFfiHtmlNamespace, parent: usize) -> usize {
        let local_name = adjusted_foreign_tag_name(token.tag_name(), namespace_);
        self.insert_element_for_with_name(token, namespace_, local_name, parent)
    }

    fn insert_element_for_with_name(
        &mut self,
        token: &Token,
        namespace_: RustFfiHtmlNamespace,
        local_name: &str,
        parent: usize,
    ) -> usize {
        self.flush_character_insertions();
        let attributes = attributes_from_token(token, namespace_);
        let owned_attributes = owned_attributes_from_token(token, namespace_);
        let element = self.create_element(namespace_, local_name, &attributes.0);
        drop(attributes);
        self.append_child(parent, element);
        let template_content = if namespace_ == RustFfiHtmlNamespace::Html && local_name == "template" {
            Some(self.template_content(element))
        } else {
            None
        };
        self.stack_of_open_elements.push(StackNode {
            handle: element,
            local_name: local_name.to_string(),
            namespace_,
            attributes: owned_attributes,
            template_content,
        });
        element
    }

    fn insert_html_element_for_active_formatting_element(
        &mut self,
        entry: &ActiveFormattingElement,
        parent: usize,
    ) -> usize {
        self.flush_character_insertions();
        let attributes = attributes_from_owned_attributes(&entry.attributes);
        let element = self.create_element(RustFfiHtmlNamespace::Html, &entry.local_name, &attributes.0);
        drop(attributes);
        self.append_child(parent, element);
        self.stack_of_open_elements.push(StackNode {
            handle: element,
            local_name: entry.local_name.clone(),
            namespace_: RustFfiHtmlNamespace::Html,
            attributes: entry.attributes.clone(),
            template_content: None,
        });
        element
    }

    fn create_element(
        &mut self,
        namespace_: RustFfiHtmlNamespace,
        local_name: &str,
        attributes: &[RustFfiHtmlParserAttribute],
    ) -> usize {
        unsafe {
            ladybird_html_parser_create_element(
                self.host,
                namespace_,
                local_name.as_ptr(),
                local_name.len(),
                attributes.as_ptr(),
                attributes.len(),
            )
        }
    }

    fn create_document_type(&mut self, name: &str, public_id: &str, system_id: &str) -> usize {
        unsafe {
            ladybird_html_parser_create_document_type(
                self.host,
                name.as_ptr(),
                name.len(),
                public_id.as_ptr(),
                public_id.len(),
                system_id.as_ptr(),
                system_id.len(),
            )
        }
    }

    fn create_comment(&mut self, data: &str) -> usize {
        unsafe { ladybird_html_parser_create_comment(self.host, data.as_ptr(), data.len()) }
    }

    fn create_text_node(&mut self, data: &str) -> usize {
        unsafe { ladybird_html_parser_create_text_node(self.host, data.as_ptr(), data.len()) }
    }

    fn append_child(&mut self, parent: usize, child: usize) {
        unsafe { ladybird_html_parser_append_child(parent, child) }
    }

    fn insert_before(&mut self, parent: usize, child: usize, before: usize) {
        unsafe { ladybird_html_parser_insert_before(parent, child, before) }
    }

    fn template_content(&mut self, element: usize) -> usize {
        unsafe { ladybird_html_parser_template_content(element) }
    }

    fn attach_declarative_shadow_root(
        &mut self,
        host: usize,
        mode: RustFfiHtmlShadowRootMode,
        clonable: bool,
        serializable: bool,
        delegates_focus: bool,
        keep_custom_element_registry_null: bool,
    ) -> usize {
        unsafe {
            ladybird_html_parser_attach_declarative_shadow_root(
                host,
                mode,
                clonable,
                serializable,
                delegates_focus,
                keep_custom_element_registry_null,
            )
        }
    }

    fn try_to_start_declarative_shadow_root(&mut self, token: &Token) -> Option<usize> {
        let mode = match token.attribute("shadowrootmode")? {
            "open" => RustFfiHtmlShadowRootMode::Open,
            "closed" => RustFfiHtmlShadowRootMode::Closed,
            _ => return None,
        };

        if self.stack_of_open_elements.len() <= 1 {
            return None;
        }

        let host = self.current_node_handle();
        let shadow_root = self.attach_declarative_shadow_root(
            host,
            mode,
            token.has_attribute("shadowrootclonable"),
            token.has_attribute("shadowrootserializable"),
            token.has_attribute("shadowrootdelegatesfocus"),
            token.has_attribute("shadowrootcustomelementregistry"),
        );
        if shadow_root == 0 {
            return None;
        }
        Some(shadow_root)
    }

    fn set_document_quirks_mode(&mut self, mode: RustFfiHtmlQuirksMode) {
        unsafe { ladybird_html_parser_set_document_quirks_mode(self.host, mode) }
    }

    fn insert_comment(&mut self, data: &str) {
        let parent = self.current_insertion_parent_handle();
        self.append_comment_to_node(parent, data);
    }

    fn append_comment_to_node(&mut self, parent: usize, data: &str) {
        self.flush_character_insertions();
        let comment = self.create_comment(data);
        self.append_child(parent, comment);
    }

    fn insert_character(&mut self, code_point: u32) {
        if let Some(character) = char::from_u32(code_point) {
            self.pending_text.push(character);
        }
    }

    fn foster_parent_character(&mut self, code_point: u32) {
        self.flush_character_insertions();
        let Some(character) = char::from_u32(code_point) else {
            return;
        };
        let data = character.to_string();
        let text = self.create_text_node(&data);
        let (parent, before) = self.foster_parenting_location();
        if before != 0 {
            self.insert_before(parent, text, before);
        } else {
            self.append_child(parent, text);
        }
    }

    fn flush_character_insertions(&mut self) {
        if self.pending_text.is_empty() {
            return;
        }
        let data = std::mem::take(&mut self.pending_text);
        let text = self.create_text_node(&data);
        self.append_child(self.current_insertion_parent_handle(), text);
    }

    fn pop_current_node(&mut self) {
        self.flush_character_insertions();
        self.stack_of_open_elements.pop();
    }

    fn pop_until_tag_name_has_been_popped(&mut self, tag_name: &str) {
        self.flush_character_insertions();
        while let Some(node) = self.stack_of_open_elements.pop() {
            if node.local_name == tag_name {
                break;
            }
        }
    }

    fn pop_until_one_of_tag_names_has_been_popped(&mut self, tag_names: &[&str]) {
        self.flush_character_insertions();
        while let Some(node) = self.stack_of_open_elements.pop() {
            if tag_names.contains(&node.local_name.as_str()) {
                break;
            }
        }
    }

    fn has_in_button_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if node.namespace_ == RustFfiHtmlNamespace::Html
                && matches!(
                    node.local_name.as_str(),
                    "applet"
                        | "caption"
                        | "html"
                        | "table"
                        | "td"
                        | "th"
                        | "marquee"
                        | "object"
                        | "template"
                        | "button"
                )
            {
                return false;
            }
        }
        false
    }

    fn has_in_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if node.namespace_ == RustFfiHtmlNamespace::Html
                && matches!(
                    node.local_name.as_str(),
                    "applet" | "caption" | "html" | "table" | "td" | "th" | "marquee" | "object" | "template"
                )
            {
                return false;
            }
        }
        false
    }

    fn has_in_list_item_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if node.namespace_ == RustFfiHtmlNamespace::Html
                && matches!(
                    node.local_name.as_str(),
                    "applet"
                        | "caption"
                        | "html"
                        | "table"
                        | "td"
                        | "th"
                        | "marquee"
                        | "object"
                        | "template"
                        | "ol"
                        | "ul"
                )
            {
                return false;
            }
        }
        false
    }

    fn current_node_is_heading(&self) -> bool {
        self.stack_of_open_elements.last().is_some_and(|node| {
            node.namespace_ == RustFfiHtmlNamespace::Html
                && matches!(node.local_name.as_str(), "h1" | "h2" | "h3" | "h4" | "h5" | "h6")
        })
    }

    fn close_a_p_element(&mut self) {
        self.pop_until_tag_name_has_been_popped("p");
    }

    fn foster_parenting_location(&self) -> (usize, usize) {
        if let Some(table_index) = self
            .stack_of_open_elements
            .iter()
            .rposition(|node| node.local_name == "table" && node.namespace_ == RustFfiHtmlNamespace::Html)
            && table_index > 0
        {
            let parent_node = &self.stack_of_open_elements[table_index - 1];
            let parent = parent_node.template_content.unwrap_or(parent_node.handle);
            let before = self.stack_of_open_elements[table_index].handle;
            return (parent, before);
        }
        (
            self.stack_of_open_elements.first().map(|node| node.handle).unwrap_or(0),
            0,
        )
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#reconstruct-the-active-formatting-elements
    fn reconstruct_the_active_formatting_elements(&mut self) {
        let mut first_missing_index = None;
        for (index, entry) in self.list_of_active_formatting_elements.iter().enumerate().rev() {
            if self
                .stack_of_open_elements
                .iter()
                .any(|node| node.handle == entry.handle)
            {
                break;
            }
            first_missing_index = Some(index);
        }

        let Some(first_missing_index) = first_missing_index else {
            return;
        };

        for index in first_missing_index..self.list_of_active_formatting_elements.len() {
            let entry = self.list_of_active_formatting_elements[index].clone();
            let element =
                self.insert_html_element_for_active_formatting_element(&entry, self.current_insertion_parent_handle());
            self.list_of_active_formatting_elements[index].handle = element;
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#adoptionAgency
    fn run_the_adoption_agency_algorithm(&mut self, tag_name: &str) {
        if self.current_node_named(tag_name) {
            let handle = self.current_node_handle();
            self.pop_current_node();
            self.remove_active_formatting_element(handle);
            return;
        }

        let Some(formatting_index) = self
            .list_of_active_formatting_elements
            .iter()
            .rposition(|entry| entry.local_name == tag_name)
        else {
            self.pop_until_tag_name_has_been_popped(tag_name);
            return;
        };

        let formatting_element = self.list_of_active_formatting_elements[formatting_index].clone();
        let Some(stack_index) = self
            .stack_of_open_elements
            .iter()
            .position(|node| node.handle == formatting_element.handle)
        else {
            self.list_of_active_formatting_elements.remove(formatting_index);
            return;
        };

        let nodes_to_reconstruct = self.stack_of_open_elements[stack_index + 1..].to_vec();
        self.stack_of_open_elements.truncate(stack_index);
        self.list_of_active_formatting_elements.remove(formatting_index);

        for node in nodes_to_reconstruct {
            if !is_formatting_element(&node.local_name) {
                continue;
            }

            let entry = ActiveFormattingElement {
                handle: node.handle,
                local_name: node.local_name,
                attributes: node.attributes,
            };
            let new_element =
                self.insert_html_element_for_active_formatting_element(&entry, self.current_insertion_parent_handle());
            self.replace_active_formatting_element(entry.handle, new_element);
        }
    }

    fn remove_active_formatting_element(&mut self, handle: usize) {
        self.list_of_active_formatting_elements
            .retain(|entry| entry.handle != handle);
    }

    fn replace_active_formatting_element(&mut self, old_handle: usize, new_handle: usize) {
        if let Some(entry) = self
            .list_of_active_formatting_elements
            .iter_mut()
            .find(|entry| entry.handle == old_handle)
        {
            entry.handle = new_handle;
        }
    }
}

struct AttributeStorage {
    local_name_bytes: Vec<Vec<u8>>,
    value_bytes: Vec<Vec<u8>>,
}

fn owned_attributes_from_token(token: &Token, namespace_: RustFfiHtmlNamespace) -> Vec<OwnedAttribute> {
    let TokenPayload::Tag {
        attributes: token_attributes,
        ..
    } = &token.payload
    else {
        return Vec::new();
    };

    let mut attributes = Vec::with_capacity(token_attributes.len());
    for attribute in token_attributes {
        let local_name = if attribute.local_name_id != 0 {
            interned_names::attr_name_by_id(attribute.local_name_id)
                .unwrap_or_default()
                .to_vec()
        } else {
            attribute.local_name.as_bytes().to_vec()
        };
        attributes.push(OwnedAttribute {
            local_name: adjusted_foreign_attribute_name(&local_name, namespace_),
            value: attribute.value.clone(),
        });
    }
    attributes
}

fn attributes_from_owned_attributes(
    attributes: &[OwnedAttribute],
) -> (Vec<RustFfiHtmlParserAttribute>, AttributeStorage) {
    let mut storage = AttributeStorage {
        local_name_bytes: Vec::with_capacity(attributes.len()),
        value_bytes: Vec::with_capacity(attributes.len()),
    };
    let mut ffi_attributes = Vec::with_capacity(attributes.len());

    for attribute in attributes {
        storage.local_name_bytes.push(attribute.local_name.as_bytes().to_vec());
        storage.value_bytes.push(attribute.value.as_bytes().to_vec());

        let local_name_bytes = storage.local_name_bytes.last().unwrap();
        let value_bytes = storage.value_bytes.last().unwrap();
        ffi_attributes.push(RustFfiHtmlParserAttribute {
            local_name_ptr: local_name_bytes.as_ptr(),
            local_name_len: local_name_bytes.len(),
            value_ptr: value_bytes.as_ptr(),
            value_len: value_bytes.len(),
        });
    }

    (ffi_attributes, storage)
}

fn attributes_from_token(
    token: &Token,
    namespace_: RustFfiHtmlNamespace,
) -> (Vec<RustFfiHtmlParserAttribute>, AttributeStorage) {
    let mut storage = AttributeStorage {
        local_name_bytes: Vec::new(),
        value_bytes: Vec::new(),
    };
    let mut attributes = Vec::new();
    let TokenPayload::Tag {
        attributes: token_attributes,
        ..
    } = &token.payload
    else {
        return (attributes, storage);
    };

    storage.local_name_bytes.reserve(token_attributes.len());
    storage.value_bytes.reserve(token_attributes.len());
    attributes.reserve(token_attributes.len());

    for attribute in token_attributes {
        let local_name = if attribute.local_name_id != 0 {
            interned_names::attr_name_by_id(attribute.local_name_id)
                .unwrap_or_default()
                .to_vec()
        } else {
            attribute.local_name.as_bytes().to_vec()
        };
        let local_name = adjusted_foreign_attribute_name(&local_name, namespace_)
            .as_bytes()
            .to_vec();
        storage.local_name_bytes.push(local_name);
        storage.value_bytes.push(attribute.value.as_bytes().to_vec());

        let local_name_bytes = storage.local_name_bytes.last().unwrap();
        let value_bytes = storage.value_bytes.last().unwrap();
        attributes.push(RustFfiHtmlParserAttribute {
            local_name_ptr: local_name_bytes.as_ptr(),
            local_name_len: local_name_bytes.len(),
            value_ptr: value_bytes.as_ptr(),
            value_len: value_bytes.len(),
        });
    }

    (attributes, storage)
}

fn adjusted_foreign_tag_name(tag_name: &str, namespace_: RustFfiHtmlNamespace) -> &str {
    if namespace_ != RustFfiHtmlNamespace::Svg {
        return tag_name;
    }
    match tag_name {
        "altglyph" => "altGlyph",
        "altglyphdef" => "altGlyphDef",
        "altglyphitem" => "altGlyphItem",
        "animatecolor" => "animateColor",
        "animatemotion" => "animateMotion",
        "animatetransform" => "animateTransform",
        "clippath" => "clipPath",
        "feblend" => "feBlend",
        "fecolormatrix" => "feColorMatrix",
        "fecomponenttransfer" => "feComponentTransfer",
        "fecomposite" => "feComposite",
        "feconvolvematrix" => "feConvolveMatrix",
        "fediffuselighting" => "feDiffuseLighting",
        "fedisplacementmap" => "feDisplacementMap",
        "fedistantlight" => "feDistantLight",
        "fedropshadow" => "feDropShadow",
        "feflood" => "feFlood",
        "fefunca" => "feFuncA",
        "fefuncb" => "feFuncB",
        "fefuncg" => "feFuncG",
        "fefuncr" => "feFuncR",
        "fegaussianblur" => "feGaussianBlur",
        "feimage" => "feImage",
        "femerge" => "feMerge",
        "femergenode" => "feMergeNode",
        "femorphology" => "feMorphology",
        "feoffset" => "feOffset",
        "fepointlight" => "fePointLight",
        "fespecularlighting" => "feSpecularLighting",
        "fespotlight" => "feSpotLight",
        "fetile" => "feTile",
        "feturbulence" => "feTurbulence",
        "foreignobject" => "foreignObject",
        "glyphref" => "glyphRef",
        "lineargradient" => "linearGradient",
        "radialgradient" => "radialGradient",
        "textpath" => "textPath",
        _ => tag_name,
    }
}

fn adjusted_foreign_end_tag_name(tag_name: &str) -> &str {
    adjusted_foreign_tag_name(tag_name, RustFfiHtmlNamespace::Svg)
}

fn adjusted_foreign_attribute_name(attribute_name: &[u8], namespace_: RustFfiHtmlNamespace) -> String {
    let Ok(attribute_name) = std::str::from_utf8(attribute_name) else {
        return String::new();
    };
    if namespace_ != RustFfiHtmlNamespace::Svg {
        return attribute_name.to_string();
    }
    match attribute_name {
        "attributename" => "attributeName",
        "attributetype" => "attributeType",
        "basefrequency" => "baseFrequency",
        "baseprofile" => "baseProfile",
        "calcmode" => "calcMode",
        "clippathunits" => "clipPathUnits",
        "contentscripttype" => "contentScriptType",
        "contentstyletype" => "contentStyleType",
        "diffuseconstant" => "diffuseConstant",
        "edgemode" => "edgeMode",
        "externalresourcesrequired" => "externalResourcesRequired",
        "filterres" => "filterRes",
        "filterunits" => "filterUnits",
        "glyphref" => "glyphRef",
        "gradienttransform" => "gradientTransform",
        "gradientunits" => "gradientUnits",
        "kernelmatrix" => "kernelMatrix",
        "kernelunitlength" => "kernelUnitLength",
        "keypoints" => "keyPoints",
        "keysplines" => "keySplines",
        "keytimes" => "keyTimes",
        "lengthadjust" => "lengthAdjust",
        "limitingconeangle" => "limitingConeAngle",
        "markerheight" => "markerHeight",
        "markerunits" => "markerUnits",
        "markerwidth" => "markerWidth",
        "maskcontentunits" => "maskContentUnits",
        "maskunits" => "maskUnits",
        "numoctaves" => "numOctaves",
        "pathlength" => "pathLength",
        "patterncontentunits" => "patternContentUnits",
        "patterntransform" => "patternTransform",
        "patternunits" => "patternUnits",
        "pointsatx" => "pointsAtX",
        "pointsaty" => "pointsAtY",
        "pointsatz" => "pointsAtZ",
        "preservealpha" => "preserveAlpha",
        "preserveaspectratio" => "preserveAspectRatio",
        "primitiveunits" => "primitiveUnits",
        "refx" => "refX",
        "refy" => "refY",
        "repeatcount" => "repeatCount",
        "repeatdur" => "repeatDur",
        "requiredextensions" => "requiredExtensions",
        "requiredfeatures" => "requiredFeatures",
        "specularconstant" => "specularConstant",
        "specularexponent" => "specularExponent",
        "spreadmethod" => "spreadMethod",
        "startoffset" => "startOffset",
        "stddeviation" => "stdDeviation",
        "stitchtiles" => "stitchTiles",
        "surfacescale" => "surfaceScale",
        "systemlanguage" => "systemLanguage",
        "tablevalues" => "tableValues",
        "targetx" => "targetX",
        "targety" => "targetY",
        "textlength" => "textLength",
        "viewbox" => "viewBox",
        "viewtarget" => "viewTarget",
        "xchannelselector" => "xChannelSelector",
        "ychannelselector" => "yChannelSelector",
        "zoomandpan" => "zoomAndPan",
        _ => attribute_name,
    }
    .to_string()
}

fn is_formatting_element(tag_name: &str) -> bool {
    matches!(
        tag_name,
        "a" | "b" | "big" | "code" | "em" | "font" | "i" | "nobr" | "s" | "small" | "strike" | "strong" | "tt" | "u"
    )
}

fn is_void_html_element(tag_name: &str) -> bool {
    matches!(
        tag_name,
        "area"
            | "base"
            | "br"
            | "col"
            | "embed"
            | "hr"
            | "img"
            | "input"
            | "link"
            | "meta"
            | "source"
            | "track"
            | "wbr"
    )
}

trait TokenExt {
    fn is_start_tag(&self) -> bool;
    fn is_end_tag(&self) -> bool;
    fn is_start_tag_named(&self, name: &str) -> bool;
    fn is_end_tag_named(&self, name: &str) -> bool;
    fn is_start_tag_one_of(&self, names: &[&str]) -> bool;
    fn is_end_tag_one_of(&self, names: &[&str]) -> bool;
    fn is_parser_whitespace(&self) -> bool;
    fn comment_data(&self) -> &str;
    fn doctype_data(&self) -> &crate::token::DoctypeData;
    fn attribute(&self, name: &str) -> Option<&str>;
    fn has_attribute(&self, name: &str) -> bool;
}

impl TokenExt for Token {
    fn is_start_tag(&self) -> bool {
        self.token_type == TokenType::StartTag
    }

    fn is_end_tag(&self) -> bool {
        self.token_type == TokenType::EndTag
    }

    fn is_start_tag_named(&self, name: &str) -> bool {
        self.is_start_tag() && self.tag_name() == name
    }

    fn is_end_tag_named(&self, name: &str) -> bool {
        self.is_end_tag() && self.tag_name() == name
    }

    fn is_start_tag_one_of(&self, names: &[&str]) -> bool {
        self.is_start_tag() && names.contains(&self.tag_name())
    }

    fn is_end_tag_one_of(&self, names: &[&str]) -> bool {
        self.is_end_tag() && names.contains(&self.tag_name())
    }

    fn is_parser_whitespace(&self) -> bool {
        if self.token_type != TokenType::Character {
            return false;
        }
        matches!(self.code_point, 0x09 | 0x0a | 0x0c | 0x0d | 0x20)
    }

    fn comment_data(&self) -> &str {
        let TokenPayload::Comment(data) = &self.payload else {
            return "";
        };
        data
    }

    fn doctype_data(&self) -> &crate::token::DoctypeData {
        let TokenPayload::Doctype(data) = &self.payload else {
            panic!("doctype_data called on non-doctype token");
        };
        data
    }

    fn attribute(&self, name: &str) -> Option<&str> {
        let TokenPayload::Tag { attributes, .. } = &self.payload else {
            return None;
        };
        attributes.iter().find_map(|attribute| {
            let local_name = if attribute.local_name_id != 0 {
                std::str::from_utf8(interned_names::attr_name_by_id(attribute.local_name_id)?).ok()?
            } else {
                attribute.local_name.as_str()
            };
            if local_name == name {
                Some(attribute.value.as_str())
            } else {
                None
            }
        })
    }

    fn has_attribute(&self, name: &str) -> bool {
        self.attribute(name).is_some()
    }
}

impl Token {
    fn synthetic_end_tag(tag_name: &str) -> Self {
        Self {
            token_type: TokenType::EndTag,
            payload: TokenPayload::Tag {
                tag_name: tag_name.to_string(),
                tag_name_id: 0,
                self_closing: false,
                attributes: Vec::new(),
            },
            ..Default::default()
        }
    }
}

/// Create a new Rust HTML parser.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_parser_create() -> *mut RustFfiHtmlParserHandle {
    Box::into_raw(Box::new(RustFfiHtmlParserHandle { run_count: 0 }))
}

/// Run the Rust HTML parser.
///
/// This is intentionally small while the C++ host side is being carved out:
/// it proves the selectable Rust parser object is linked and reachable, and
/// gives the next step a stable ABI to extend with DOM host callbacks.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_run_document(
    handle: *mut RustFfiHtmlParserHandle,
    tokenizer: *mut RustFfiTokenizerHandle,
    host: *mut c_void,
    stop_at_insertion_point: bool,
) -> RustFfiHtmlParserRunResult {
    if handle.is_null() || tokenizer.is_null() || host.is_null() {
        return RustFfiHtmlParserRunResult::Unsupported;
    }
    let handle = unsafe { &mut *handle };
    handle.run_count = handle.run_count.wrapping_add(1);
    let tokenizer = unsafe { &mut *tokenizer };
    let mut tree_builder = TreeBuilder::new(&mut tokenizer.tokenizer, host);
    tree_builder.run(stop_at_insertion_point);
    RustFfiHtmlParserRunResult::Ok
}

/// Return how many times this parser handle has been asked to run.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_run_count(handle: *const RustFfiHtmlParserHandle) -> u64 {
    if handle.is_null() {
        return 0;
    }
    let handle = unsafe { &*handle };
    handle.run_count
}

/// Destroy a Rust HTML parser.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`,
/// and must not be used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_destroy(handle: *mut RustFfiHtmlParserHandle) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}
