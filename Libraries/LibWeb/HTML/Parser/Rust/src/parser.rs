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
use std::ops::{Deref, DerefMut};
use std::ptr::{NonNull, addr_of_mut};

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlParserRunResult {
    Ok = 0,
    Unsupported = 1,
    ExecuteScript = 2,
    ExecuteSvgScript = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlNamespace {
    Html = 0,
    MathMl = 1,
    Svg = 2,
    Other = 3,
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
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlSlotAssignmentMode {
    Named = 0,
    Manual = 1,
}

struct DeclarativeShadowRootInit {
    host: usize,
    mode: RustFfiHtmlShadowRootMode,
    slot_assignment: RustFfiHtmlSlotAssignmentMode,
    clonable: bool,
    serializable: bool,
    delegates_focus: bool,
    keep_custom_element_registry_null: bool,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlAttributeNamespace {
    None = 0,
    XLink = 1,
    Xml = 2,
    Xmlns = 3,
    Other = 4,
}

#[repr(C)]
pub struct RustFfiHtmlParserAttribute {
    pub local_name_ptr: *const u8,
    pub local_name_len: usize,
    pub prefix_ptr: *const u8,
    pub prefix_len: usize,
    pub namespace_: RustFfiHtmlAttributeNamespace,
    pub value_ptr: *const u8,
    pub value_len: usize,
}

unsafe extern "C" {
    fn ladybird_html_parser_document_node(parser: *mut c_void) -> usize;
    fn ladybird_html_parser_document_html_element(parser: *mut c_void) -> usize;
    fn ladybird_html_parser_set_document_quirks_mode(parser: *mut c_void, mode: RustFfiHtmlQuirksMode);
    fn ladybird_html_parser_log_parse_error(parser: *mut c_void, message_ptr: *const u8, message_len: usize);
    fn ladybird_html_parser_stop_parsing(parser: *mut c_void);
    fn ladybird_html_parser_parse_errors_enabled() -> bool;
    fn ladybird_html_parser_visit_node(visitor: *mut c_void, node: usize);
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
    fn ladybird_html_parser_insert_text(parent: usize, before: usize, data_ptr: *const u8, data_len: usize);
    fn ladybird_html_parser_add_missing_attribute(
        element: usize,
        local_name_ptr: *const u8,
        local_name_len: usize,
        value_ptr: *const u8,
        value_len: usize,
    );
    fn ladybird_html_parser_remove_node(node: usize);
    fn ladybird_html_parser_handle_element_popped(element: usize);
    fn ladybird_html_parser_prepare_svg_script(parser: *mut c_void, element: usize, source_line_number: usize);
    fn ladybird_html_parser_set_script_source_line(parser: *mut c_void, element: usize, source_line_number: usize);
    fn ladybird_html_parser_mark_script_already_started(parser: *mut c_void, element: usize);
    fn ladybird_html_parser_parent_node(node: usize) -> usize;
    fn ladybird_html_parser_create_element(
        parser: *mut c_void,
        intended_parent: usize,
        namespace_: RustFfiHtmlNamespace,
        namespace_uri_ptr: *const u8,
        namespace_uri_len: usize,
        local_name_ptr: *const u8,
        local_name_len: usize,
        attributes: *const RustFfiHtmlParserAttribute,
        attribute_count: usize,
        had_duplicate_attribute: bool,
        form_element: usize,
        has_template_element_on_stack: bool,
    ) -> usize;
    fn ladybird_html_parser_append_child(parent: usize, child: usize);
    fn ladybird_html_parser_insert_node(
        parent: usize,
        before: usize,
        child: usize,
        queue_custom_element_reactions: bool,
    );
    fn ladybird_html_parser_move_all_children(from: usize, to: usize);
    fn ladybird_html_parser_template_content(element: usize) -> usize;
    fn ladybird_html_parser_attach_declarative_shadow_root(
        host: usize,
        mode: RustFfiHtmlShadowRootMode,
        slot_assignment: RustFfiHtmlSlotAssignmentMode,
        clonable: bool,
        serializable: bool,
        delegates_focus: bool,
        keep_custom_element_registry_null: bool,
    ) -> usize;
    fn ladybird_html_parser_set_template_content(element: usize, content: usize);
    fn ladybird_html_parser_allows_declarative_shadow_roots(node: usize) -> bool;
}

/// Opaque handle for the Rust HTML parser, passed across the FFI boundary.
pub struct RustFfiHtmlParserHandle {
    run_count: u64,
    state: ParserState,
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
    InTemplate,
    InTableText,
    InTable,
    InCaption,
    InColumnGroup,
    InTableBody,
    InRow,
    InCell,
    InFrameset,
    AfterFrameset,
    AfterBody,
    AfterAfterBody,
    AfterAfterFrameset,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum AdoptionAgencyAlgorithmOutcome {
    DoNothing,
    RunAnyOtherEndTagSteps,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct OwnedAttribute {
    local_name: String,
    prefix: Option<String>,
    namespace_: RustFfiHtmlAttributeNamespace,
    value: String,
}

#[derive(Clone, Debug)]
struct StackNode {
    handle: usize,
    local_name: String,
    namespace_: RustFfiHtmlNamespace,
    namespace_uri: Option<String>,
    attributes: Vec<OwnedAttribute>,
    template_content: Option<usize>,
}

struct FragmentParsingContext {
    root: usize,
    context_element: StackNode,
    document_quirks_mode: RustFfiHtmlQuirksMode,
    form_element: usize,
}

#[derive(Clone, Debug)]
struct ActiveFormattingElement {
    handle: usize,
    local_name: String,
    attributes: Vec<OwnedAttribute>,
    is_marker: bool,
}

const QUIRKS_PUBLIC_IDS: &[&str] = &[
    "+//Silmaril//dtd html Pro v0r11 19970101//",
    "-//AS//DTD HTML 3.0 asWedit + extensions//",
    "-//AdvaSoft Ltd//DTD HTML 3.0 asWedit + extensions//",
    "-//IETF//DTD HTML 2.0 Level 1//",
    "-//IETF//DTD HTML 2.0 Level 2//",
    "-//IETF//DTD HTML 2.0 Strict Level 1//",
    "-//IETF//DTD HTML 2.0 Strict Level 2//",
    "-//IETF//DTD HTML 2.0 Strict//",
    "-//IETF//DTD HTML 2.0//",
    "-//IETF//DTD HTML 2.1E//",
    "-//IETF//DTD HTML 3.0//",
    "-//IETF//DTD HTML 3.2 Final//",
    "-//IETF//DTD HTML 3.2//",
    "-//IETF//DTD HTML 3//",
    "-//IETF//DTD HTML Level 0//",
    "-//IETF//DTD HTML Level 1//",
    "-//IETF//DTD HTML Level 2//",
    "-//IETF//DTD HTML Level 3//",
    "-//IETF//DTD HTML Strict Level 0//",
    "-//IETF//DTD HTML Strict Level 1//",
    "-//IETF//DTD HTML Strict Level 2//",
    "-//IETF//DTD HTML Strict Level 3//",
    "-//IETF//DTD HTML Strict//",
    "-//IETF//DTD HTML//",
    "-//Metrius//DTD Metrius Presentational//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 2.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 2.0 Tables//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML Strict//",
    "-//Microsoft//DTD Internet Explorer 3.0 HTML//",
    "-//Microsoft//DTD Internet Explorer 3.0 Tables//",
    "-//Netscape Comm. Corp.//DTD HTML//",
    "-//Netscape Comm. Corp.//DTD Strict HTML//",
    "-//O'Reilly and Associates//DTD HTML 2.0//",
    "-//O'Reilly and Associates//DTD HTML Extended 1.0//",
    "-//O'Reilly and Associates//DTD HTML Extended Relaxed 1.0//",
    "-//SQ//DTD HTML 2.0 HoTMetaL + extensions//",
    "-//SoftQuad Software//DTD HoTMetaL PRO 6.0::19990601::extensions to HTML 4.0//",
    "-//SoftQuad//DTD HoTMetaL PRO 4.0::19971010::extensions to HTML 4.0//",
    "-//Spyglass//DTD HTML 2.0 Extended//",
    "-//Sun Microsystems Corp.//DTD HotJava HTML//",
    "-//Sun Microsystems Corp.//DTD HotJava Strict HTML//",
    "-//W3C//DTD HTML 3 1995-03-24//",
    "-//W3C//DTD HTML 3.2 Draft//",
    "-//W3C//DTD HTML 3.2 Final//",
    "-//W3C//DTD HTML 3.2//",
    "-//W3C//DTD HTML 3.2S Draft//",
    "-//W3C//DTD HTML 4.0 Frameset//",
    "-//W3C//DTD HTML 4.0 Transitional//",
    "-//W3C//DTD HTML Experimental 19960712//",
    "-//W3C//DTD HTML Experimental 970421//",
    "-//W3C//DTD W3 HTML//",
    "-//W3O//DTD W3 HTML 3.0//",
    "-//WebTechs//DTD Mozilla HTML 2.0//",
    "-//WebTechs//DTD Mozilla HTML//",
];

struct ParserState {
    stack_of_open_elements: Vec<StackNode>,
    list_of_active_formatting_elements: Vec<ActiveFormattingElement>,
    stack_of_template_insertion_modes: Vec<InsertionMode>,
    insertion_mode: InsertionMode,
    original_insertion_mode: InsertionMode,
    document_quirks_mode: RustFfiHtmlQuirksMode,
    head_element: Option<usize>,
    form_element: Option<usize>,
    parsing_fragment: bool,
    context_element: Option<StackNode>,
    scripting_enabled: bool,
    next_line_feed_can_be_ignored: bool,
    pending_text: String,
    pending_table_text: String,
    pending_table_text_contains_non_whitespace: bool,
    pending_script: Option<usize>,
    pending_svg_script: Option<usize>,
    parser_pause_requested: bool,
    foster_parenting_enabled: bool,
    frameset_ok: bool,
}

impl ParserState {
    fn new() -> Self {
        Self {
            stack_of_open_elements: Vec::new(),
            list_of_active_formatting_elements: Vec::new(),
            stack_of_template_insertion_modes: Vec::new(),
            insertion_mode: InsertionMode::Initial,
            original_insertion_mode: InsertionMode::Initial,
            document_quirks_mode: RustFfiHtmlQuirksMode::No,
            head_element: None,
            form_element: None,
            parsing_fragment: false,
            context_element: None,
            scripting_enabled: true,
            next_line_feed_can_be_ignored: false,
            pending_text: String::new(),
            pending_table_text: String::new(),
            pending_table_text_contains_non_whitespace: false,
            pending_script: None,
            pending_svg_script: None,
            parser_pause_requested: false,
            foster_parenting_enabled: false,
            frameset_ok: true,
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-html-fragments
    fn begin_fragment(&mut self, fragment_context: FragmentParsingContext) {
        let context_namespace = fragment_context.context_element.namespace_;
        let context_local_name = fragment_context.context_element.local_name.clone();
        *self = Self::new();
        self.parsing_fragment = true;
        self.document_quirks_mode = fragment_context.document_quirks_mode;
        self.context_element = Some(fragment_context.context_element);

        // 13. Set up the HTML parser's stack of open elements so that it contains just the single element root.
        self.stack_of_open_elements.push(StackNode {
            handle: fragment_context.root,
            local_name: "html".to_string(),
            namespace_: RustFfiHtmlNamespace::Html,
            namespace_uri: None,
            attributes: Vec::new(),
            template_content: None,
        });
        if fragment_context.form_element != 0 {
            self.form_element = Some(fragment_context.form_element);
        }
        // 14. If context is a template element, then push "in template" onto the stack of template insertion modes
        //     so that it is the new current template insertion mode.
        if context_namespace == RustFfiHtmlNamespace::Html && context_local_name == "template" {
            self.stack_of_template_insertion_modes.push(InsertionMode::InTemplate);
        }
        // 15. Create a start tag token whose name is the local name of context and whose attributes are the attributes
        //     of context.
        // The synthetic context token is represented by ParserState::context_element.

        // 16. Reset the parser's insertion mode appropriately.
        self.reset_the_insertion_mode_appropriately();
        self.original_insertion_mode = self.insertion_mode;
    }

    fn visit_edges(&self, visitor: *mut c_void) {
        for node in &self.stack_of_open_elements {
            visit_node(visitor, node.handle);
            if let Some(template_content) = node.template_content {
                visit_node(visitor, template_content);
            }
        }

        for element in &self.list_of_active_formatting_elements {
            visit_node(visitor, element.handle);
        }

        visit_node(visitor, self.head_element.unwrap_or(0));
        visit_node(visitor, self.form_element.unwrap_or(0));
        if let Some(context_element) = &self.context_element {
            visit_node(visitor, context_element.handle);
        }
        visit_node(visitor, self.pending_script.unwrap_or(0));
        visit_node(visitor, self.pending_svg_script.unwrap_or(0));
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#reset-the-insertion-mode-appropriately
    fn reset_the_insertion_mode_appropriately(&mut self) {
        let mut last = false;

        for index in (0..self.stack_of_open_elements.len()).rev() {
            if index == 0 {
                last = true;
            }

            let node = if last && self.parsing_fragment {
                self.context_element
                    .as_ref()
                    .unwrap_or(&self.stack_of_open_elements[index])
            } else {
                &self.stack_of_open_elements[index]
            };

            if node.namespace_ == RustFfiHtmlNamespace::Html {
                if !last && matches!(node.local_name.as_str(), "td" | "th") {
                    self.insertion_mode = InsertionMode::InCell;
                    return;
                }

                if node.local_name == "tr" {
                    self.insertion_mode = InsertionMode::InRow;
                    return;
                }

                if matches!(node.local_name.as_str(), "tbody" | "thead" | "tfoot") {
                    self.insertion_mode = InsertionMode::InTableBody;
                    return;
                }

                if node.local_name == "caption" {
                    self.insertion_mode = InsertionMode::InCaption;
                    return;
                }

                if node.local_name == "colgroup" {
                    self.insertion_mode = InsertionMode::InColumnGroup;
                    return;
                }

                if node.local_name == "table" {
                    self.insertion_mode = InsertionMode::InTable;
                    return;
                }

                if node.local_name == "template" {
                    self.insertion_mode = self
                        .stack_of_template_insertion_modes
                        .last()
                        .copied()
                        .unwrap_or(InsertionMode::InTemplate);
                    return;
                }

                if !last && node.local_name == "head" {
                    self.insertion_mode = InsertionMode::InHead;
                    return;
                }

                if node.local_name == "body" {
                    self.insertion_mode = InsertionMode::InBody;
                    return;
                }

                if node.local_name == "frameset" {
                    self.insertion_mode = InsertionMode::InFrameset;
                    return;
                }

                if node.local_name == "html" {
                    if self.head_element.is_none() {
                        self.insertion_mode = InsertionMode::BeforeHead;
                    } else {
                        self.insertion_mode = InsertionMode::AfterHead;
                    }
                    return;
                }
            }

            if last {
                self.insertion_mode = InsertionMode::InBody;
                return;
            }
        }
    }
}

fn visit_node(visitor: *mut c_void, node: usize) {
    if node != 0 {
        unsafe { ladybird_html_parser_visit_node(visitor, node) };
    }
}

struct TreeBuilder {
    tokenizer: NonNull<HtmlTokenizer>,
    host: *mut c_void,
    state: NonNull<ParserState>,
    log_parse_errors: bool,
}

impl Deref for TreeBuilder {
    type Target = ParserState;

    fn deref(&self) -> &Self::Target {
        // SAFETY: TreeBuilder is only constructed with a ParserState pointer owned by the
        // C++ HTMLParser handle, and the handle outlives each parser run.
        unsafe { self.state.as_ref() }
    }
}

impl DerefMut for TreeBuilder {
    fn deref_mut(&mut self) -> &mut Self::Target {
        // SAFETY: TreeBuilder is only constructed with a ParserState pointer owned by the
        // C++ HTMLParser handle, and the handle outlives each parser run.
        unsafe { self.state.as_mut() }
    }
}

impl TreeBuilder {
    fn new(tokenizer: NonNull<HtmlTokenizer>, host: *mut c_void, state: NonNull<ParserState>) -> Self {
        let log_parse_errors = unsafe { ladybird_html_parser_parse_errors_enabled() };
        Self {
            tokenizer,
            host,
            state,
            log_parse_errors,
        }
    }

    fn next_token(&mut self, stop_at_insertion_point: bool, cdata_allowed: bool) -> Option<Token> {
        // SAFETY: TreeBuilder is only constructed with a tokenizer pointer owned by the C++
        // HTMLParser's Rust tokenizer handle, and the handle outlives each parser run.
        unsafe {
            self.tokenizer
                .as_mut()
                .next_token(stop_at_insertion_point, cdata_allowed)
        }
    }

    fn switch_tokenizer_to(&mut self, state: State) {
        // SAFETY: TreeBuilder is only constructed with a tokenizer pointer owned by the C++
        // HTMLParser's Rust tokenizer handle, and the handle outlives each parser run.
        unsafe { self.tokenizer.as_mut().switch_to(state) };
    }

    fn run(&mut self, stop_at_insertion_point: bool) {
        loop {
            let cdata_allowed = self
                .adjusted_current_node()
                .is_some_and(|node| node.namespace_ != RustFfiHtmlNamespace::Html);
            let Some(token) = self.next_token(stop_at_insertion_point, cdata_allowed) else {
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
            if self.pending_script.is_some() || self.pending_svg_script.is_some() || self.parser_pause_requested {
                break;
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
            InsertionMode::InTemplate => self.handle_in_template(token),
            InsertionMode::InTableText => self.handle_in_table_text(token),
            InsertionMode::InTable => self.handle_in_table(token),
            InsertionMode::InCaption => self.handle_in_caption(token),
            InsertionMode::InColumnGroup => self.handle_in_column_group(token),
            InsertionMode::InTableBody => self.handle_in_table_body(token),
            InsertionMode::InRow => self.handle_in_row(token),
            InsertionMode::InCell => self.handle_in_cell(token),
            InsertionMode::InFrameset => self.handle_in_frameset(token),
            InsertionMode::AfterFrameset => self.handle_after_frameset(token),
            InsertionMode::AfterBody => self.handle_after_body(token),
            InsertionMode::AfterAfterBody => self.handle_after_after_body(token),
            InsertionMode::AfterAfterFrameset => self.handle_after_after_frameset(token),
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#tree-construction-dispatcher
    fn should_process_token_using_html_rules(&self, token: &Token) -> bool {
        if self.stack_of_open_elements.is_empty() || token.token_type == TokenType::EndOfFile {
            return true;
        }

        let Some(adjusted_current_node) = self.adjusted_current_node() else {
            return true;
        };

        if adjusted_current_node.namespace_ == RustFfiHtmlNamespace::Html {
            return true;
        }

        if is_mathml_text_integration_point(adjusted_current_node) {
            if token.is_start_tag() && !token.is_start_tag_one_of(&["mglyph", "malignmark"]) {
                return true;
            }
            if token.token_type == TokenType::Character {
                return true;
            }
        }

        if adjusted_current_node.namespace_ == RustFfiHtmlNamespace::MathMl
            && adjusted_current_node.local_name == "annotation-xml"
            && token.is_start_tag_named("svg")
        {
            return true;
        }

        if is_html_integration_point(adjusted_current_node)
            && (token.is_start_tag() || token.token_type == TokenType::Character)
        {
            return true;
        }

        false
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
            // If the DOCTYPE token's name is not "html", or the token's public identifier is not missing,
            // or the token's system identifier is neither missing nor "about:legacy-compat", then there is a parse error.
            let doctype = token.doctype_data();
            if doctype.name != "html"
                || !doctype.missing_public_identifier
                || (!doctype.missing_system_identifier && doctype.system_identifier != "about:legacy-compat")
            {
                self.parse_error("unexpected DOCTYPE token");
            }

            // Append a DocumentType node to the Document node, with its name set to the name given in the DOCTYPE token,
            // or the empty string if the name was missing; its public ID set to the public identifier given in the DOCTYPE token,
            // or the empty string if the public identifier was missing; and its system ID set to the system identifier
            // given in the DOCTYPE token, or the empty string if the system identifier was missing.
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
        // FIXME: Expose whether the document is an iframe srcdoc document so this parse error can be conditional.
        self.parse_error("anything else in initial insertion mode");
        self.set_document_quirks_mode(RustFfiHtmlQuirksMode::Yes);
        self.insertion_mode = InsertionMode::BeforeHtml;
        self.process_using_the_rules_for(InsertionMode::BeforeHtml, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-before-html-insertion-mode
    fn handle_before_html(&mut self, token: Token) {
        // -> A DOCTYPE token
        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in before html insertion mode");
            return;
        }

        // -> A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF),
        //    U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), or U+0020 SPACE
        if token.is_parser_whitespace() {
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
            if let Some(html_element) = self.document_html_element() {
                self.stack_of_open_elements.push(StackNode {
                    handle: html_element,
                    local_name: "html".to_string(),
                    namespace_: RustFfiHtmlNamespace::Html,
                    namespace_uri: None,
                    attributes: Vec::new(),
                    template_content: None,
                });
            } else {
                self.insert_html_element_named("html", document);
            }
            self.insertion_mode = InsertionMode::BeforeHead;
            self.process_using_the_rules_for(InsertionMode::BeforeHead, token);
            return;
        }

        // -> Any other end tag
        if token.is_end_tag() {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in before html insertion mode");
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-before-head-insertion-mode
    fn handle_before_head(&mut self, token: Token) {
        // -> A character token that is one of U+0009 CHARACTER TABULATION, U+000A LINE FEED (LF),
        //    U+000C FORM FEED (FF), U+000D CARRIAGE RETURN (CR), or U+0020 SPACE
        if token.is_parser_whitespace() {
            return;
        }

        // -> A DOCTYPE token
        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in before head insertion mode");
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
            return;
        }

        // -> Any other end tag
        if token.is_end_tag() {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in before head insertion mode");
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
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in in head insertion mode");
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

        if token.is_start_tag_one_of(&["style", "noframes"])
            || (token.is_start_tag_named("noscript") && self.scripting_enabled)
        {
            self.parse_generic_raw_text_element(token);
            return;
        }

        if token.is_start_tag_named("noscript") && !self.scripting_enabled {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InHeadNoscript;
            return;
        }

        if token.is_start_tag_named("template") {
            self.handle_template_start_tag(&token);
            return;
        }

        if token.is_start_tag_named("script") {
            // Run these steps:
            //
            // 1. Let the adjusted insertion location be the appropriate place for inserting a node.
            // 2. Create an element for the token in the HTML namespace, with the intended parent being the element in
            //    which the adjusted insertion location finds itself.
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            // 9. Switch the tokenizer to the script data state.
            self.switch_tokenizer_to(State::ScriptData);
            // 10. Set the original insertion mode to the current insertion mode.
            self.original_insertion_mode = self.insertion_mode;
            // 11. Switch the insertion mode to "text".
            self.insertion_mode = InsertionMode::Text;
            return;
        }

        if token.is_end_tag_named("head") {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::AfterHead;
            return;
        }

        if token.is_end_tag_named("template") {
            self.handle_template_end_tag();
            return;
        }

        if token.is_end_tag_one_of(&["body", "html", "br"]) {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::AfterHead;
            self.process_using_the_rules_for(InsertionMode::AfterHead, token);
            return;
        }

        if token.is_start_tag_named("head") || token.is_end_tag() {
            // Parse error. Ignore the token.
            self.parse_error("unexpected tag in in head insertion mode");
            return;
        }

        // Pop the current node (which will be the head element) off the stack of open elements.
        self.pop_current_node();
        // Switch the insertion mode to "after head".
        self.insertion_mode = InsertionMode::AfterHead;
        // Reprocess the token.
        self.process_using_the_rules_for(InsertionMode::AfterHead, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inheadnoscript
    fn handle_in_head_noscript(&mut self, token: Token) {
        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in in head noscript insertion mode");
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_end_tag_named("noscript") {
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InHead;
            return;
        }

        if token.is_parser_whitespace()
            || token.token_type == TokenType::Comment
            || token.is_start_tag_one_of(&["basefont", "bgsound", "link", "meta", "noframes", "style"])
        {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_one_of(&["head", "noscript"]) || (token.is_end_tag() && !token.is_end_tag_named("br")) {
            // Parse error. Ignore the token.
            self.parse_error("unexpected tag in in head noscript insertion mode");
            return;
        }

        // Parse error.
        self.parse_error("anything else in in head noscript insertion mode");

        // Pop the current node (which will be a noscript element) from the stack of open elements; the new current node will be a head element.
        self.pop_current_node();
        // Switch the insertion mode to "in head".
        self.insertion_mode = InsertionMode::InHead;
        // Reprocess the token.
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
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in after head insertion mode");
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_named("body") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.frameset_ok = false;
            self.insertion_mode = InsertionMode::InBody;
            return;
        }

        if token.is_start_tag_named("frameset") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InFrameset;
            return;
        }

        if token.is_start_tag_one_of(&[
            "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title",
        ]) {
            // Parse error.
            self.parse_error("head-only start tag in after head insertion mode");
            if let Some(head) = self.head_element {
                self.stack_of_open_elements.push(StackNode {
                    handle: head,
                    local_name: "head".to_string(),
                    namespace_: RustFfiHtmlNamespace::Html,
                    namespace_uri: None,
                    attributes: Vec::new(),
                    template_content: None,
                });
                self.process_using_the_rules_for(InsertionMode::InHead, token);
                self.stack_of_open_elements.retain(|node| node.handle != head);
            }
            return;
        }

        if token.is_end_tag_named("template") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        // An end tag whose tag name is one of: "body", "html", "br"
        if token.is_end_tag_one_of(&["body", "html", "br"]) {
            self.insert_html_element_named("body", self.current_node_handle());
            self.insertion_mode = InsertionMode::InBody;
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        // A start tag whose tag name is "head"
        // Any other end tag
        if token.is_start_tag_named("head") || token.is_end_tag() {
            // Parse error. Ignore the token.
            self.parse_error("unexpected tag in after head insertion mode");
            return;
        }

        // Anything else
        {
            // Insert an HTML element for a "body" start tag token with no attributes.
            self.insert_html_element_named("body", self.current_node_handle());
            // Switch the insertion mode to "in body".
            self.insertion_mode = InsertionMode::InBody;
            // Reprocess the current token.
            self.process_using_the_rules_for(InsertionMode::InBody, token);
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody
    fn handle_in_body(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            if token.code_point == 0 {
                // Parse error. Ignore the token.
                self.parse_error("U+0000 character token in in body insertion mode");
                return;
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_character(token.code_point);
            if !token.is_parser_whitespace() {
                self.frameset_ok = false;
            }
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in in body insertion mode");
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            if !self.stack_of_template_insertion_modes.is_empty() {
                self.process_using_the_rules_for(InsertionMode::InTemplate, token);
                return;
            }
            self.parse_error_if_stack_contains_unexpected_node_at_end_of_body();
            self.stop_parsing();
            return;
        }

        if token.is_start_tag_named("html") {
            // Parse error.
            self.parse_error("html start tag in in body insertion mode");

            if !self.has_template_element_on_stack_of_open_elements() && !self.stack_of_open_elements.is_empty() {
                let html = self.stack_of_open_elements[0].handle;
                self.add_missing_attributes_to_element(html, &token);
            }
            return;
        }

        if token.is_start_tag_named("body") {
            // Parse error.
            self.parse_error("body start tag in in body insertion mode");

            if self.stack_of_open_elements.len() > 1
                && self
                    .stack_of_open_elements
                    .get(1)
                    .is_some_and(|node| node.local_name == "body" && node.namespace_ == RustFfiHtmlNamespace::Html)
                && !self.has_template_element_on_stack_of_open_elements()
            {
                let body = self.stack_of_open_elements[1].handle;
                self.add_missing_attributes_to_element(body, &token);
                self.frameset_ok = false;
            }
            return;
        }

        if token.is_start_tag_named("frameset") {
            // Parse error.
            self.parse_error("frameset start tag in in body insertion mode");

            if self.stack_of_open_elements.len() == 1
                || !self
                    .stack_of_open_elements
                    .get(1)
                    .is_some_and(|node| node.local_name == "body" && node.namespace_ == RustFfiHtmlNamespace::Html)
                || self.has_template_element_on_stack_of_open_elements()
                || !self.frameset_ok
            {
                return;
            }

            let body = self.stack_of_open_elements[1].handle;
            unsafe {
                ladybird_html_parser_remove_node(body);
            }
            while self.stack_of_open_elements.len() > 1 && !self.current_node_named("html") {
                self.pop_stack_node();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InFrameset;
            return;
        }

        if token.is_start_tag_one_of(&[
            "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "title",
        ]) {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_named("template") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_one_of(&[
            "address",
            "article",
            "aside",
            "blockquote",
            "center",
            "details",
            "dialog",
            "dir",
            "div",
            "dl",
            "fieldset",
            "figcaption",
            "figure",
            "footer",
            "header",
            "hgroup",
            "main",
            "menu",
            "nav",
            "ol",
            "search",
            "section",
            "summary",
            "ul",
        ]) {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
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
            self.frameset_ok = false;
            for node in self.stack_of_open_elements.iter().rev() {
                if node.local_name == "li" {
                    self.generate_implied_end_tags_except("li");
                    if !self.current_node_named("li") {
                        self.parse_error("current node is not li before closing li start tag");
                    }
                    self.pop_until_tag_name_has_been_popped("li");
                    break;
                }

                if is_special_tag(node) && !matches!(node.local_name.as_str(), "address" | "div" | "p") {
                    break;
                }
            }
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_one_of(&["dd", "dt"]) {
            self.frameset_ok = false;
            for node in self.stack_of_open_elements.iter().rev() {
                if node.local_name == "dd" || node.local_name == "dt" {
                    let tag_name = node.local_name.clone();
                    self.generate_implied_end_tags_except(tag_name.as_str());
                    if !self.current_node_named(tag_name.as_str()) {
                        self.parse_error("current node does not match definition-list item start tag");
                    }
                    self.pop_until_tag_name_has_been_popped(tag_name.as_str());
                    break;
                }

                if is_special_tag(node) && !matches!(node.local_name.as_str(), "address" | "div" | "p") {
                    break;
                }
            }
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("button") {
            if self.has_in_button_scope("button") {
                // Parse error.
                self.parse_error("button start tag with button element in scope");
                self.generate_implied_end_tags();
                self.pop_until_tag_name_has_been_popped("button");
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.frameset_ok = false;
            return;
        }

        if token.is_start_tag_named("form") {
            let has_template_element_on_stack = self.has_template_element_on_stack_of_open_elements();
            if self.form_element.is_some() && !has_template_element_on_stack {
                // Parse error. Ignore the token.
                self.parse_error("form start tag with non-null form element pointer");
                return;
            }
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            let form = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            if !has_template_element_on_stack {
                self.form_element = Some(form);
            }
            return;
        }

        if token.is_end_tag_named("form") {
            if !self.has_template_element_on_stack_of_open_elements() {
                let node = self.form_element.take();
                if let Some(node) = node {
                    if self.has_in_scope_by_handle(node) {
                        self.generate_implied_end_tags();
                        if self.current_node_handle() != node {
                            self.parse_error("current node is not form element pointer");
                        }
                        self.stack_of_open_elements.retain(|entry| entry.handle != node);
                    } else {
                        // Parse error. Ignore the token.
                        self.parse_error("form end tag without form element pointer in scope");
                    }
                } else {
                    // Parse error. Ignore the token.
                    self.parse_error("form end tag without form element pointer");
                }
                return;
            }

            if !self.has_in_scope("form") {
                // Parse error. Ignore the token.
                self.parse_error("form end tag without form element in scope");
                return;
            }
            self.generate_implied_end_tags();
            if !self.current_node_named("form") {
                self.parse_error("current node is not form element");
            }
            self.pop_until_tag_name_has_been_popped("form");
            return;
        }

        // An end tag whose tag name is "li"
        if token.is_end_tag_named("li") {
            if !self.has_in_list_item_scope("li") {
                // Parse error. Ignore the token.
                self.parse_error("li end tag without li element in list item scope");
                return;
            }

            self.generate_implied_end_tags_except("li");
            if !self.current_node_named("li") {
                self.parse_error("current node is not li element");
            }
            self.pop_until_tag_name_has_been_popped("li");
            return;
        }

        // An end tag whose tag name is one of: "dd", "dt"
        if token.is_end_tag_one_of(&["dd", "dt"]) {
            if !self.has_in_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("definition-list item end tag without matching element in scope");
                return;
            }

            self.generate_implied_end_tags_except(token.tag_name());
            if !self.current_node_named(token.tag_name()) {
                self.parse_error("current node does not match definition-list item end tag");
            }
            self.pop_until_tag_name_has_been_popped(token.tag_name());
            return;
        }

        if token.is_start_tag_one_of(&["h1", "h2", "h3", "h4", "h5", "h6"]) {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            if self.current_node_is_heading() {
                // Parse error.
                self.parse_error("heading start tag with heading element as current node");
                self.pop_current_node();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        // An end tag whose tag name is one of: "h1", "h2", "h3", "h4", "h5", "h6"
        if token.is_end_tag_one_of(&["h1", "h2", "h3", "h4", "h5", "h6"]) {
            if !self.has_heading_in_scope() {
                // Parse error. Ignore the token.
                self.parse_error("heading end tag without heading element in scope");
                return;
            }

            self.generate_implied_end_tags();
            if !self.current_node_named(token.tag_name()) {
                self.parse_error("current node does not match heading end tag");
            }
            self.pop_until_heading_has_been_popped();
            return;
        }

        if token.is_start_tag_one_of(&["pre", "listing"]) {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.next_line_feed_can_be_ignored = true;
            self.frameset_ok = false;
            return;
        }

        if token.is_start_tag_named("plaintext") {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.switch_tokenizer_to(State::PLAINTEXT);
            return;
        }

        // A start tag whose tag name is "ruby"
        if token.is_start_tag_named("ruby") {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("image") {
            // Parse error. Change the token's tag name to "img" and reprocess it. (Don't ask.)
            self.parse_error("image start tag in in body insertion mode");
            let mut token = token;
            *token.tag_name_mut() = "img".to_string();
            self.process_using_the_rules_for(self.insertion_mode, token);
            return;
        }

        if token.is_start_tag_named("table") {
            if self.document_quirks_mode != RustFfiHtmlQuirksMode::Yes && self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.frameset_ok = false;
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_start_tag_named("select") {
            if self.parsing_fragment
                && self.context_element.as_ref().is_some_and(|element| {
                    element.local_name == "select" && element.namespace_ == RustFfiHtmlNamespace::Html
                })
            {
                // Parse error. Ignore the token.
                self.parse_error("select start tag in select fragment");
                return;
            }
            if self.has_in_scope("select") {
                // Parse error.
                self.parse_error("select start tag with select element in scope");
                self.pop_until_tag_name_has_been_popped("select");
                return;
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.frameset_ok = false;
            return;
        }

        if token.is_end_tag_named("template") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_named("svg") {
            self.reconstruct_the_active_formatting_elements();
            self.insert_foreign_element_for(&token, RustFfiHtmlNamespace::Svg);
            if token.is_self_closing() {
                self.pop_current_node();
            }
            return;
        }

        if token.is_start_tag_named("math") {
            self.reconstruct_the_active_formatting_elements();
            self.insert_foreign_element_for(&token, RustFfiHtmlNamespace::MathMl);
            if token.is_self_closing() {
                self.pop_current_node();
            }
            return;
        }

        if token.is_start_tag_one_of(&[
            "caption", "col", "colgroup", "frame", "head", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) {
            // Parse error. Ignore the token.
            self.parse_error("table-related start tag in in body insertion mode");
            return;
        }

        if token.is_start_tag_named("option") {
            if self.has_in_scope("select") {
                self.generate_implied_end_tags_except("optgroup");
                if self.has_in_scope("option") {
                    self.parse_error("option start tag with option element in scope");
                }
            } else if self.current_node_named("option") {
                self.pop_current_node();
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_named("optgroup") {
            if self.has_in_scope("select") {
                self.generate_implied_end_tags();
                if self.has_in_scope("option") || self.has_in_scope("optgroup") {
                    self.parse_error("optgroup start tag with option or optgroup element in scope");
                }
            } else if self.current_node_named("option") {
                self.pop_current_node();
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        // A start tag whose tag name is one of: "rb", "rtc"
        if token.is_start_tag_one_of(&["rb", "rtc"]) {
            // If the stack of open elements has a ruby element in scope, then generate implied end tags. If the current node is not now a ruby element, this is a parse error.
            if self.has_in_scope("ruby") {
                self.generate_implied_end_tags();
            }
            if !self.current_node_named("ruby") {
                self.parse_error("ruby child start tag without ruby as current node");
            }

            // Insert an HTML element for the token.
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        // A start tag whose tag name is one of: "rp", "rt"
        if token.is_start_tag_one_of(&["rp", "rt"]) {
            // If the stack of open elements has a ruby element in scope, then generate implied end tags, except for rtc elements. If the current node is not now a rtc element or a ruby element, this is a parse error.
            if self.has_in_scope("ruby") {
                self.generate_implied_end_tags_except("rtc");
            }
            if !self.current_node_named("rtc") && !self.current_node_named("ruby") {
                self.parse_error("ruby text start tag without rtc or ruby as current node");
            }

            // Insert an HTML element for the token.
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            return;
        }

        if token.is_start_tag_one_of(&["textarea"]) {
            self.next_line_feed_can_be_ignored = true;
            self.parse_generic_rcdata_element(token);
            self.frameset_ok = false;
            return;
        }

        if token.is_start_tag_named("xmp") {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            self.reconstruct_the_active_formatting_elements();
            self.frameset_ok = false;
            self.parse_generic_raw_text_element(token);
            return;
        }

        if token.is_start_tag_named("iframe") {
            self.frameset_ok = false;
            self.parse_generic_raw_text_element(token);
            return;
        }

        if token.is_start_tag_one_of(&["style", "noembed", "noframes"])
            || (token.is_start_tag_named("noscript") && self.scripting_enabled)
        {
            self.parse_generic_raw_text_element(token);
            return;
        }

        if token.is_start_tag_named("hr") {
            if self.has_in_button_scope("p") {
                self.close_a_p_element();
            }
            if self.has_in_scope("select") {
                self.generate_implied_end_tags();
                if self.has_in_scope("option") || self.has_in_scope("optgroup") {
                    self.parse_error("hr start tag with option or optgroup element in scope");
                }
            }
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            self.frameset_ok = false;
            return;
        }

        if token.is_start_tag_named("br") {
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            self.frameset_ok = false;
            return;
        }

        if token.is_start_tag_named("input") {
            if self.parsing_fragment
                && self.context_element.as_ref().is_some_and(|element| {
                    element.local_name == "select" && element.namespace_ == RustFfiHtmlNamespace::Html
                })
            {
                // Parse error. Ignore the token.
                self.parse_error("input start tag in select fragment");
                return;
            }
            if self.has_in_scope("select") {
                // Parse error.
                self.parse_error("input start tag with select element in scope");
                self.pop_until_tag_name_has_been_popped("select");
            }
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            if !token
                .attribute("type")
                .is_some_and(|value| value.eq_ignore_ascii_case("hidden"))
            {
                self.frameset_ok = false;
            }
            return;
        }

        if token.is_end_tag_named("p") {
            if !self.has_in_button_scope("p") {
                // Parse error.
                self.parse_error("p end tag without p element in button scope");
                self.insert_html_element_named("p", self.current_insertion_parent_handle());
            }
            self.close_a_p_element();
            return;
        }

        if token.is_end_tag_named("br") {
            // Parse error.
            self.parse_error("br end tag in in body insertion mode");
            self.process_using_the_rules_for(self.insertion_mode, Token::synthetic_start_tag("br"));
            return;
        }

        if token.is_end_tag_one_of(&[
            "address",
            "article",
            "aside",
            "blockquote",
            "button",
            "center",
            "details",
            "dialog",
            "dir",
            "div",
            "dl",
            "fieldset",
            "figcaption",
            "figure",
            "footer",
            "header",
            "hgroup",
            "listing",
            "main",
            "menu",
            "nav",
            "ol",
            "pre",
            "search",
            "section",
            "select",
            "summary",
            "ul",
        ]) {
            if !self.has_in_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("end tag without matching element in scope");
                return;
            }
            self.generate_implied_end_tags();
            if !self.current_node_named(token.tag_name()) {
                self.parse_error("current node does not match end tag");
            }
            self.pop_until_tag_name_has_been_popped(token.tag_name());
            return;
        }

        if token.is_start_tag_named("a") {
            let active_formatting_elements_after_last_marker =
                self.active_formatting_elements_after_last_marker_start_index();
            if let Some(active_index) = self
                .list_of_active_formatting_elements
                .iter()
                .enumerate()
                .skip(active_formatting_elements_after_last_marker)
                .rposition(|(_, entry)| entry.local_name == "a")
                .map(|index| active_formatting_elements_after_last_marker + index)
            {
                // Parse error.
                self.parse_error("a start tag with active a formatting element");
                let active_element = self.list_of_active_formatting_elements[active_index].handle;
                if self.run_the_adoption_agency_algorithm("a") == AdoptionAgencyAlgorithmOutcome::RunAnyOtherEndTagSteps
                {
                    self.process_any_other_end_tag("a");
                    return;
                }
                self.remove_active_formatting_element(active_element);
                self.stack_of_open_elements.retain(|node| node.handle != active_element);
            }
            self.reconstruct_the_active_formatting_elements();
            let element = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.push_onto_the_list_of_active_formatting_elements(element, &token);
            return;
        }

        if token.is_start_tag_named("nobr") {
            self.reconstruct_the_active_formatting_elements();
            if self.has_in_scope("nobr") {
                // Parse error.
                self.parse_error("nobr start tag with nobr element in scope");
                self.run_the_adoption_agency_algorithm("nobr");
                self.reconstruct_the_active_formatting_elements();
            }
            let element = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.push_onto_the_list_of_active_formatting_elements(element, &token);
            return;
        }

        if token.is_start_tag() && is_formatting_element(token.tag_name()) {
            self.reconstruct_the_active_formatting_elements();
            let element = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.push_onto_the_list_of_active_formatting_elements(element, &token);
            return;
        }

        if token.is_end_tag() && is_formatting_element(token.tag_name()) {
            if self.run_the_adoption_agency_algorithm(token.tag_name())
                == AdoptionAgencyAlgorithmOutcome::RunAnyOtherEndTagSteps
            {
                self.process_any_other_end_tag(token.tag_name());
            }
            return;
        }

        if token.is_start_tag_one_of(&["applet", "marquee", "object"]) {
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insert_marker_at_the_end_of_the_list_of_active_formatting_elements();
            self.frameset_ok = false;
            return;
        }

        if token.is_end_tag_one_of(&["applet", "marquee", "object"]) {
            if !self.has_in_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("end tag without matching applet, marquee, or object element in scope");
                return;
            }
            self.generate_implied_end_tags();
            if !self.current_node_named(token.tag_name()) {
                self.parse_error("current node does not match applet, marquee, or object end tag");
            }
            self.pop_until_tag_name_has_been_popped(token.tag_name());
            self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
            return;
        }

        if token.is_start_tag_one_of(&["param", "source", "track"]) {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_start_tag() {
            self.reconstruct_the_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            if is_void_html_element(token.tag_name()) {
                self.pop_current_node();
            }
            if matches!(token.tag_name(), "area" | "embed" | "img" | "keygen" | "wbr") {
                self.frameset_ok = false;
            }
            return;
        }

        if token.is_end_tag_named("body") {
            if !self.has_in_scope("body") {
                // Parse error. Ignore the token.
                self.parse_error("body end tag without body element in scope");
                return;
            }
            self.parse_error_if_stack_contains_unexpected_node_at_end_of_body();
            self.insertion_mode = InsertionMode::AfterBody;
            return;
        }

        if token.is_end_tag_named("html") {
            if !self.has_in_scope("body") {
                // Parse error. Ignore the token.
                self.parse_error("html end tag without body element in scope");
                return;
            }
            self.parse_error_if_stack_contains_unexpected_node_at_end_of_body();
            self.insertion_mode = InsertionMode::AfterBody;
            self.process_using_the_rules_for(InsertionMode::AfterBody, token);
            return;
        }

        if token.is_end_tag() {
            self.process_any_other_end_tag(token.tag_name());
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

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in in frameset insertion mode");
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
            if self.stack_of_open_elements.len() == 1 {
                // Parse error. Ignore the token.
                self.parse_error("frameset end tag with only root element on stack");
                return;
            }
            self.pop_current_node();
            if !self.parsing_fragment && !self.current_node_named("frameset") {
                self.insertion_mode = InsertionMode::AfterFrameset;
            }
            return;
        }

        if token.is_start_tag_named("frame") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_start_tag_named("noframes") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            if !self.current_node_named("html") {
                self.parse_error("end of file in in frameset insertion mode with non-root current node");
            }
            self.stop_parsing();
            return;
        }

        // Parse error. Ignore the token.
        self.parse_error("unexpected token in in frameset insertion mode");
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-afterframeset
    fn handle_after_frameset(&mut self, token: Token) {
        if token.is_parser_whitespace() {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in after frameset insertion mode");
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_end_tag_named("html") {
            self.insertion_mode = InsertionMode::AfterAfterFrameset;
            return;
        }

        if token.is_start_tag_named("noframes") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            self.stop_parsing();
            return;
        }

        // Parse error. Ignore the token.
        self.parse_error("unexpected token in after frameset insertion mode");
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intable
    fn handle_in_table(&mut self, token: Token) {
        if token.token_type == TokenType::Character
            && self.current_node_is_one_of(&["table", "tbody", "template", "tfoot", "thead", "tr"])
        {
            self.pending_table_text.clear();
            self.pending_table_text_contains_non_whitespace = false;
            self.original_insertion_mode = self.insertion_mode;
            self.insertion_mode = InsertionMode::InTableText;
            self.process_using_the_rules_for(InsertionMode::InTableText, token);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in in table insertion mode");
            return;
        }

        if token.is_start_tag_one_of(&["script", "style", "template"]) || token.is_end_tag_named("template") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_named("input")
            && token
                .attribute("type")
                .is_some_and(|type_| type_.eq_ignore_ascii_case("hidden"))
        {
            // Parse error.
            self.parse_error("hidden input start tag in in table insertion mode");
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_start_tag_named("form") {
            // Parse error.
            self.parse_error("form start tag in in table insertion mode");
            if self.form_element.is_some() || self.has_template_element_on_stack_of_open_elements() {
                return;
            }
            let form = self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.form_element = Some(form);
            self.pop_current_node();
            return;
        }

        if token.is_start_tag_one_of(&["tbody", "tfoot", "thead"]) {
            self.clear_the_stack_back_to_a_table_context();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTableBody;
            return;
        }

        if token.is_start_tag_named("caption") {
            self.clear_the_stack_back_to_a_table_context();
            self.insert_marker_at_the_end_of_the_list_of_active_formatting_elements();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InCaption;
            return;
        }

        if token.is_start_tag_named("colgroup") {
            self.clear_the_stack_back_to_a_table_context();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InColumnGroup;
            return;
        }

        if token.is_start_tag_named("col") {
            self.clear_the_stack_back_to_a_table_context();
            self.insert_html_element_named("colgroup", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InColumnGroup;
            self.process_using_the_rules_for(InsertionMode::InColumnGroup, token);
            return;
        }

        if token.is_start_tag_named("tr") {
            self.clear_the_stack_back_to_a_table_context();
            self.insert_html_element_named("tbody", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTableBody;
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_start_tag_one_of(&["td", "th"]) {
            self.clear_the_stack_back_to_a_table_context();
            self.insert_html_element_named("tbody", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InTableBody;
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_start_tag_named("table") {
            // Parse error.
            self.parse_error("table start tag in in table insertion mode");
            if !self.has_in_table_scope("table") {
                return;
            }
            self.pop_until_tag_name_has_been_popped("table");
            self.reset_the_insertion_mode_appropriately();
            self.process_using_the_rules_for(self.insertion_mode, token);
            return;
        }

        if token.is_end_tag_named("table") {
            if !self.has_in_table_scope("table") {
                // Parse error. Ignore the token.
                self.parse_error("table end tag without table element in table scope");
                return;
            }
            self.pop_until_tag_name_has_been_popped("table");
            self.reset_the_insertion_mode_appropriately();
            return;
        }

        if token.is_end_tag_one_of(&[
            "body", "caption", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in in table insertion mode");
            return;
        }

        // Parse error.
        self.parse_error("anything else in in table insertion mode");
        let old_foster_parenting_enabled = self.foster_parenting_enabled;
        self.foster_parenting_enabled = true;
        self.process_using_the_rules_for(InsertionMode::InBody, token);
        self.flush_character_insertions();
        self.foster_parenting_enabled = old_foster_parenting_enabled;
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intabletext
    fn handle_in_table_text(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            if token.code_point == 0 {
                // Parse error. Ignore the token.
                self.parse_error("U+0000 character token in in table text insertion mode");
                return;
            }
            if !token.is_parser_whitespace() {
                self.pending_table_text_contains_non_whitespace = true;
            }
            if let Some(character) = char::from_u32(token.code_point) {
                self.pending_table_text.push(character);
            }
            return;
        }

        let pending_table_text = std::mem::take(&mut self.pending_table_text);
        if self.pending_table_text_contains_non_whitespace {
            // Parse error.
            self.parse_error("non-whitespace character token in table text insertion mode");
            let old_foster_parenting_enabled = self.foster_parenting_enabled;
            self.foster_parenting_enabled = true;
            for character in pending_table_text.chars() {
                self.process_using_the_rules_for(InsertionMode::InBody, Token::synthetic_character(character as u32));
                self.flush_character_insertions();
            }
            self.foster_parenting_enabled = old_foster_parenting_enabled;
        } else {
            for character in pending_table_text.chars() {
                self.insert_character(character as u32);
            }
            self.flush_character_insertions();
        }
        self.pending_table_text_contains_non_whitespace = false;
        self.insertion_mode = self.original_insertion_mode;
        self.process_using_the_rules_for(self.insertion_mode, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-incolgroup
    fn handle_in_column_group(&mut self, token: Token) {
        if token.token_type == TokenType::Character && token.is_parser_whitespace() {
            self.insert_character(token.code_point);
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in in column group insertion mode");
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_named("col") {
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.pop_current_node();
            return;
        }

        if token.is_end_tag_named("colgroup") {
            if !self.current_node_named("colgroup") {
                // Parse error. Ignore the token.
                self.parse_error("colgroup end tag without colgroup as current node");
                return;
            }
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_end_tag_named("col") {
            // Parse error. Ignore the token.
            self.parse_error("col end tag in in column group insertion mode");
            return;
        }

        if token.is_start_tag_named("template") || token.is_end_tag_named("template") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if !self.current_node_named("colgroup") {
            // Parse error. Ignore the token.
            self.parse_error("anything else in in column group insertion mode without colgroup as current node");
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InColumnGroup, Token::synthetic_end_tag("colgroup"));
        self.process_using_the_rules_for(InsertionMode::InTable, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-incaption
    fn handle_in_caption(&mut self, token: Token) {
        if token.is_end_tag_named("caption") {
            if !self.has_in_table_scope("caption") {
                // Parse error. Ignore the token.
                self.parse_error("caption end tag without caption element in table scope");
                return;
            }
            self.generate_implied_end_tags();
            if !self.current_node_named("caption") {
                self.parse_error("current node is not caption element");
            }
            self.pop_until_tag_name_has_been_popped("caption");
            self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_start_tag_one_of(&[
            "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) || token.is_end_tag_named("table")
        {
            if !self.has_in_table_scope("caption") {
                // Parse error. Ignore the token.
                self.parse_error("table token in caption insertion mode without caption element in table scope");
                return;
            }
            self.generate_implied_end_tags();
            if !self.current_node_named("caption") {
                self.parse_error("current node is not caption element");
            }
            self.pop_until_tag_name_has_been_popped("caption");
            self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
            self.insertion_mode = InsertionMode::InTable;
            self.process_using_the_rules_for(InsertionMode::InTable, token);
            return;
        }

        if token.is_end_tag_one_of(&[
            "body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in in caption insertion mode");
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intbody
    fn handle_in_table_body(&mut self, token: Token) {
        if token.is_start_tag_one_of(&["caption", "col", "colgroup", "tbody", "tfoot", "thead"])
            || token.is_end_tag_named("table")
        {
            if !self.has_in_table_scope("tbody")
                && !self.has_in_table_scope("thead")
                && !self.has_in_table_scope("tfoot")
            {
                // Parse error. Ignore the token.
                self.parse_error("table body boundary token without table body element in table scope");
                return;
            }
            self.clear_the_stack_back_to_a_table_body_context();
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InTable;
            self.process_using_the_rules_for(InsertionMode::InTable, token);
            return;
        }

        if token.is_start_tag_named("tr") {
            self.clear_the_stack_back_to_a_table_body_context();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InRow;
            return;
        }

        if token.is_start_tag_one_of(&["td", "th"]) {
            // Parse error.
            self.parse_error("cell start tag in in table body insertion mode");
            self.clear_the_stack_back_to_a_table_body_context();
            self.insert_html_element_named("tr", self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InRow;
            self.process_using_the_rules_for(InsertionMode::InRow, token);
            return;
        }

        if token.is_end_tag_one_of(&["tbody", "tfoot", "thead"]) {
            if !self.has_in_table_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("table body end tag without matching element in table scope");
                return;
            }
            self.clear_the_stack_back_to_a_table_body_context();
            self.pop_current_node();
            self.insertion_mode = InsertionMode::InTable;
            return;
        }

        if token.is_end_tag_one_of(&["body", "caption", "col", "colgroup", "html", "td", "th", "tr"]) {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in in table body insertion mode");
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InTable, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intr
    fn handle_in_row(&mut self, token: Token) {
        if token.is_start_tag_one_of(&["td", "th"]) {
            self.clear_the_stack_back_to_a_table_row_context();
            self.insert_html_element_for(&token, self.current_insertion_parent_handle());
            self.insertion_mode = InsertionMode::InCell;
            self.insert_marker_at_the_end_of_the_list_of_active_formatting_elements();
            return;
        }

        if token.is_end_tag_named("tr") {
            if !self.has_in_table_scope("tr") {
                // Parse error. Ignore the token.
                self.parse_error("tr end tag without tr element in table scope");
                return;
            }
            self.clear_the_stack_back_to_a_table_row_context();
            self.pop_until_tag_name_has_been_popped("tr");
            self.insertion_mode = InsertionMode::InTableBody;
            return;
        }

        if token.is_start_tag_one_of(&["caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"])
            || token.is_end_tag_named("table")
        {
            if !self.has_in_table_scope("tr") {
                // Parse error. Ignore the token.
                self.parse_error("table token in row insertion mode without tr element in table scope");
                return;
            }
            self.process_using_the_rules_for(InsertionMode::InRow, Token::synthetic_end_tag("tr"));
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_end_tag_one_of(&["tbody", "tfoot", "thead"]) {
            if !self.has_in_table_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("table body end tag in row insertion mode without matching element in table scope");
                return;
            }
            if !self.has_in_table_scope("tr") {
                return;
            }
            self.process_using_the_rules_for(InsertionMode::InRow, Token::synthetic_end_tag("tr"));
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_end_tag_one_of(&["body", "caption", "col", "colgroup", "html", "td", "th"]) {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in in row insertion mode");
            return;
        }

        self.process_using_the_rules_for(InsertionMode::InTable, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intd
    fn handle_in_cell(&mut self, token: Token) {
        if token.is_end_tag_one_of(&["td", "th"]) {
            if !self.has_in_table_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("cell end tag without matching element in table scope");
                return;
            }
            self.generate_implied_end_tags();
            if !self.current_node_named(token.tag_name()) {
                self.parse_error("current node does not match cell end tag");
            }
            self.pop_until_tag_name_has_been_popped(token.tag_name());
            self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
            self.insertion_mode = InsertionMode::InRow;
            return;
        }

        if token.is_start_tag_one_of(&[
            "caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr",
        ]) {
            if !self.has_in_table_scope("td") && !self.has_in_table_scope("th") {
                // Parse error. Ignore the token.
                self.parse_error("table token in cell insertion mode without cell element in table scope");
                return;
            }
            self.close_the_cell();
            self.process_using_the_rules_for(self.insertion_mode, token);
            return;
        }

        if token.is_end_tag_one_of(&["body", "caption", "col", "colgroup", "html"]) {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in in cell insertion mode");
            return;
        }

        if token.is_end_tag_one_of(&["table", "tbody", "tfoot", "thead", "tr"]) {
            if !self.has_in_table_scope(token.tag_name()) {
                // Parse error. Ignore the token.
                self.parse_error("table end tag in cell insertion mode without matching element in table scope");
                return;
            }
            self.close_the_cell();
            self.process_using_the_rules_for(self.insertion_mode, token);
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
            // Parse error.
            self.parse_error("end of file in text insertion mode");
            if self.current_node_named("script") {
                self.mark_script_already_started(self.current_node_handle());
            }
            self.pop_current_node();
            self.insertion_mode = self.original_insertion_mode;
            self.process_using_the_rules_for(self.insertion_mode, token);
            return;
        }

        if token.is_end_tag_named("script") && self.current_node_named("script") {
            self.flush_character_insertions();
            let script = self.current_node_handle();
            self.pop_current_node();
            self.insertion_mode = self.original_insertion_mode;
            // https://html.spec.whatwg.org/multipage/parsing.html#scripting-mode
            // The Fragment scripting mode treats parser-inserted scripts as if they were not parser-inserted, allowing,
            // for example, executing scripts when applying a fragment created by createContextualFragment().
            if self.parsing_fragment {
                return;
            }
            self.pending_script = Some(script);
            return;
        }

        if token.is_end_tag() {
            self.pop_current_node();
            self.insertion_mode = self.original_insertion_mode;
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-intemplate
    fn handle_in_template(&mut self, token: Token) {
        if token.token_type == TokenType::Character
            || token.token_type == TokenType::Comment
            || token.token_type == TokenType::Doctype
        {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_start_tag_one_of(&[
            "base", "basefont", "bgsound", "link", "meta", "noframes", "script", "style", "template", "title",
        ]) || token.is_end_tag_named("template")
        {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        if token.is_start_tag_one_of(&["caption", "colgroup", "tbody", "tfoot", "thead"]) {
            // Pop the current template insertion mode off the stack of template insertion modes.
            self.stack_of_template_insertion_modes.pop();
            // Push "in table" onto the stack of template insertion modes so that it is the new current template insertion mode.
            self.stack_of_template_insertion_modes.push(InsertionMode::InTable);
            // Switch the insertion mode to "in table", and reprocess the token.
            self.insertion_mode = InsertionMode::InTable;
            self.process_using_the_rules_for(InsertionMode::InTable, token);
            return;
        }

        if token.is_start_tag_named("col") {
            // Pop the current template insertion mode off the stack of template insertion modes.
            self.stack_of_template_insertion_modes.pop();
            // Push "in column group" onto the stack of template insertion modes so that it is the new current template insertion mode.
            self.stack_of_template_insertion_modes
                .push(InsertionMode::InColumnGroup);
            // Switch the insertion mode to "in column group", and reprocess the token.
            self.insertion_mode = InsertionMode::InColumnGroup;
            self.process_using_the_rules_for(InsertionMode::InColumnGroup, token);
            return;
        }

        if token.is_start_tag_named("tr") {
            // Pop the current template insertion mode off the stack of template insertion modes.
            self.stack_of_template_insertion_modes.pop();
            // Push "in table body" onto the stack of template insertion modes so that it is the new current template insertion mode.
            self.stack_of_template_insertion_modes.push(InsertionMode::InTableBody);
            // Switch the insertion mode to "in table body", and reprocess the token.
            self.insertion_mode = InsertionMode::InTableBody;
            self.process_using_the_rules_for(InsertionMode::InTableBody, token);
            return;
        }

        if token.is_start_tag_one_of(&["td", "th"]) {
            // Pop the current template insertion mode off the stack of template insertion modes.
            self.stack_of_template_insertion_modes.pop();
            // Push "in row" onto the stack of template insertion modes so that it is the new current template insertion mode.
            self.stack_of_template_insertion_modes.push(InsertionMode::InRow);
            // Switch the insertion mode to "in row", and reprocess the token.
            self.insertion_mode = InsertionMode::InRow;
            self.process_using_the_rules_for(InsertionMode::InRow, token);
            return;
        }

        if token.is_start_tag() {
            // Pop the current template insertion mode off the stack of template insertion modes.
            self.stack_of_template_insertion_modes.pop();
            // Push "in body" onto the stack of template insertion modes so that it is the new current template insertion mode.
            self.stack_of_template_insertion_modes.push(InsertionMode::InBody);
            // Switch the insertion mode to "in body", and reprocess the token.
            self.insertion_mode = InsertionMode::InBody;
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_end_tag() {
            // Parse error. Ignore the token.
            self.parse_error("unexpected end tag in in template insertion mode");
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            if !self.has_template_element_on_stack_of_open_elements() {
                self.stop_parsing();
                return;
            }

            // Parse error.
            self.parse_error("end of file in in template insertion mode");
            self.pop_until_tag_name_has_been_popped("template");
            self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
            self.stack_of_template_insertion_modes.pop();
            self.reset_the_insertion_mode_appropriately();
            self.process_using_the_rules_for(self.insertion_mode, token);
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

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in after body insertion mode");
            return;
        }

        if token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.is_end_tag_named("html") {
            if self.parsing_fragment {
                // Parse error. Ignore the token.
                self.parse_error("html end tag in after body insertion mode for fragment");
            } else {
                self.insertion_mode = InsertionMode::AfterAfterBody;
            }
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            self.stop_parsing();
            return;
        }

        // Parse error.
        self.parse_error("unexpected token in after body insertion mode");
        self.insertion_mode = InsertionMode::InBody;
        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-after-after-body-insertion-mode
    fn handle_after_after_body(&mut self, token: Token) {
        if token.token_type == TokenType::Comment {
            let document = self.document_node();
            self.append_comment_to_node(document, token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype || token.is_parser_whitespace() || token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            self.stop_parsing();
            return;
        }

        // Parse error.
        self.parse_error("unexpected token in after after body insertion mode");
        self.insertion_mode = InsertionMode::InBody;
        self.process_using_the_rules_for(InsertionMode::InBody, token);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#the-after-after-frameset-insertion-mode
    fn handle_after_after_frameset(&mut self, token: Token) {
        if token.token_type == TokenType::Comment {
            let document = self.document_node();
            self.append_comment_to_node(document, token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype || token.is_parser_whitespace() || token.is_start_tag_named("html") {
            self.process_using_the_rules_for(InsertionMode::InBody, token);
            return;
        }

        if token.token_type == TokenType::EndOfFile {
            self.stop_parsing();
            return;
        }

        if token.is_start_tag_named("noframes") {
            self.process_using_the_rules_for(InsertionMode::InHead, token);
            return;
        }

        // Parse error. Ignore the token.
        self.parse_error("unexpected token in after after frameset insertion mode");
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign
    fn process_using_the_rules_for_foreign_content(&mut self, token: Token) {
        if token.token_type == TokenType::Character {
            if token.code_point == 0 {
                // Parse error. Insert a U+FFFD REPLACEMENT CHARACTER character.
                self.parse_error("U+0000 character token in foreign content");
                self.insert_character(0xfffd);
                return;
            }
            self.insert_character(token.code_point);
            if !token.is_parser_whitespace() {
                self.frameset_ok = false;
            }
            return;
        }

        if token.token_type == TokenType::Comment {
            self.insert_comment(token.comment_data());
            return;
        }

        if token.token_type == TokenType::Doctype {
            // Parse error. Ignore the token.
            self.parse_error("DOCTYPE token in foreign content");
            return;
        }

        if is_foreign_content_breakout_token(&token) {
            // Parse error.
            self.parse_error("HTML breakout token in foreign content");
            while self.stack_of_open_elements.last().is_some_and(|node| {
                node.namespace_ != RustFfiHtmlNamespace::Html
                    && !is_mathml_text_integration_point(node)
                    && !is_html_integration_point(node)
            }) {
                self.pop_current_node();
            }
            self.process_using_the_rules_for(self.insertion_mode, token);
            return;
        }

        if token.is_start_tag() {
            let namespace_ = self
                .adjusted_current_node()
                .map(|node| node.namespace_)
                .unwrap_or(RustFfiHtmlNamespace::Html);
            let element = self.insert_foreign_element_for(&token, namespace_);
            if token.is_self_closing() {
                self.pop_current_node();
                if namespace_ == RustFfiHtmlNamespace::Svg && token.tag_name() == "script" {
                    self.process_svg_script(element);
                }
            }
            return;
        }

        if token.is_end_tag_named("script")
            && self
                .stack_of_open_elements
                .last()
                .is_some_and(|node| node.namespace_ == RustFfiHtmlNamespace::Svg && node.local_name == "script")
        {
            self.flush_character_insertions();
            let script = self.current_node_handle();
            self.pop_current_node();
            self.process_svg_script(script);
            return;
        }

        if token.is_end_tag() {
            if self.stack_of_open_elements.is_empty() {
                return;
            }

            let mut index = self.stack_of_open_elements.len() - 1;
            loop {
                if index == 0 {
                    return;
                }

                if self.stack_of_open_elements[index]
                    .local_name
                    .eq_ignore_ascii_case(token.tag_name())
                {
                    self.flush_character_insertions();
                    self.stack_of_open_elements.truncate(index);
                    return;
                }

                index -= 1;
                if self.stack_of_open_elements[index].namespace_ != RustFfiHtmlNamespace::Html {
                    continue;
                }

                self.process_using_the_rules_for(self.insertion_mode, token);
                return;
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#generic-rcdata-element-parsing-algorithm
    fn parse_generic_rcdata_element(&mut self, token: Token) {
        // 1. Insert an HTML element for the token.
        self.insert_html_element_for(&token, self.current_insertion_parent_handle());
        // 2. Switch the tokenizer to the RCDATA state.
        self.switch_tokenizer_to(State::RCDATA);
        // 3. Set the original insertion mode to the current insertion mode.
        self.original_insertion_mode = self.insertion_mode;
        // 4. Then, switch the insertion mode to "text".
        self.insertion_mode = InsertionMode::Text;
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#generic-raw-text-element-parsing-algorithm
    fn parse_generic_raw_text_element(&mut self, token: Token) {
        // 1. Insert an HTML element for the token.
        self.insert_html_element_for(&token, self.current_insertion_parent_handle());
        // 2. Switch the tokenizer to the RAWTEXT state.
        self.switch_tokenizer_to(State::RAWTEXT);
        // 3. Set the original insertion mode to the current insertion mode.
        self.original_insertion_mode = self.insertion_mode;
        // 4. Then, switch the insertion mode to "text".
        self.insertion_mode = InsertionMode::Text;
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#quirks-mode-doctypes
    fn which_quirks_mode(&self, token: &Token) -> RustFfiHtmlQuirksMode {
        let doctype = token.doctype_data();
        if doctype.force_quirks {
            return RustFfiHtmlQuirksMode::Yes;
        }
        if doctype.name != "html" {
            return RustFfiHtmlQuirksMode::Yes;
        }
        let public_identifier = doctype.public_identifier.as_str();
        let system_identifier = doctype.system_identifier.as_str();
        if doctype
            .public_identifier
            .eq_ignore_ascii_case("-//W3O//DTD W3 HTML Strict 3.0//EN//")
            || doctype
                .public_identifier
                .eq_ignore_ascii_case("-/W3C/DTD HTML 4.0 Transitional/EN")
            || doctype.public_identifier.eq_ignore_ascii_case("HTML")
            || system_identifier.eq_ignore_ascii_case("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")
        {
            return RustFfiHtmlQuirksMode::Yes;
        }
        if QUIRKS_PUBLIC_IDS
            .iter()
            .any(|public_id| starts_with_ignore_ascii_case(public_identifier, public_id))
        {
            return RustFfiHtmlQuirksMode::Yes;
        }
        if doctype.missing_system_identifier
            && (starts_with_ignore_ascii_case(public_identifier, "-//W3C//DTD HTML 4.01 Frameset//")
                || starts_with_ignore_ascii_case(public_identifier, "-//W3C//DTD HTML 4.01 Transitional//"))
        {
            return RustFfiHtmlQuirksMode::Yes;
        }
        if starts_with_ignore_ascii_case(public_identifier, "-//W3C//DTD XHTML 1.0 Frameset//")
            || starts_with_ignore_ascii_case(public_identifier, "-//W3C//DTD XHTML 1.0 Transitional//")
        {
            return RustFfiHtmlQuirksMode::Limited;
        }
        if !doctype.missing_system_identifier
            && (starts_with_ignore_ascii_case(public_identifier, "-//W3C//DTD HTML 4.01 Frameset//")
                || starts_with_ignore_ascii_case(public_identifier, "-//W3C//DTD HTML 4.01 Transitional//"))
        {
            return RustFfiHtmlQuirksMode::Limited;
        }
        RustFfiHtmlQuirksMode::No
    }

    fn document_node(&mut self) -> usize {
        unsafe { ladybird_html_parser_document_node(self.host) }
    }

    fn document_html_element(&mut self) -> Option<usize> {
        let element = unsafe { ladybird_html_parser_document_html_element(self.host) };
        (element != 0).then_some(element)
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

    fn current_node_is_one_of(&self, tag_names: &[&str]) -> bool {
        self.stack_of_open_elements.last().is_some_and(|node| {
            node.namespace_ == RustFfiHtmlNamespace::Html && tag_names.contains(&node.local_name.as_str())
        })
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#adjusted-current-node
    fn adjusted_current_node(&self) -> Option<&StackNode> {
        if self.parsing_fragment && self.stack_of_open_elements.len() == 1 {
            return self.context_element.as_ref();
        }
        self.stack_of_open_elements.last()
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-an-html-element
    fn insert_html_element_named(&mut self, name: &str, parent: usize) -> usize {
        self.flush_character_insertions();
        let (adjusted_parent, adjusted_before) = self.appropriate_place_for_inserting_node(parent);
        let element = self.create_element(adjusted_parent, RustFfiHtmlNamespace::Html, None, name, &[], false);
        self.insert_parser_created_element(adjusted_parent, adjusted_before, element);
        let template_content = if name == "template" {
            Some(self.template_content(element))
        } else {
            None
        };
        self.stack_of_open_elements.push(StackNode {
            handle: element,
            local_name: name.to_string(),
            namespace_: RustFfiHtmlNamespace::Html,
            namespace_uri: None,
            attributes: Vec::new(),
            template_content,
        });
        element
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-an-html-element
    fn insert_html_element_for(&mut self, token: &Token, parent: usize) -> usize {
        self.insert_element_for(token, RustFfiHtmlNamespace::Html, parent)
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-a-foreign-element
    fn insert_foreign_element_for(&mut self, token: &Token, namespace_: RustFfiHtmlNamespace) -> usize {
        self.insert_element_for(token, namespace_, self.current_insertion_parent_handle())
    }

    fn insert_element_for(&mut self, token: &Token, namespace_: RustFfiHtmlNamespace, parent: usize) -> usize {
        self.insert_element_for_at(token, namespace_, parent, 0)
    }

    fn insert_element_for_at(
        &mut self,
        token: &Token,
        namespace_: RustFfiHtmlNamespace,
        parent: usize,
        before: usize,
    ) -> usize {
        let local_name = adjusted_foreign_tag_name(token.tag_name(), namespace_);
        self.insert_element_for_with_name_at(token, namespace_, local_name, parent, before)
    }

    fn insert_element_for_with_name_at(
        &mut self,
        token: &Token,
        namespace_: RustFfiHtmlNamespace,
        local_name: &str,
        parent: usize,
        before: usize,
    ) -> usize {
        self.flush_character_insertions();
        // 1. Let the adjustedInsertionLocation be the appropriate place for inserting a node.
        let (adjusted_parent, adjusted_before) = if before == 0 {
            self.appropriate_place_for_inserting_node(parent)
        } else {
            (parent, before)
        };
        let attributes = attributes_from_token(token, namespace_);
        let owned_attributes = owned_attributes_from_token(token, namespace_);
        let namespace_uri = if namespace_ == RustFfiHtmlNamespace::Other {
            self.adjusted_current_node()
                .and_then(|node| node.namespace_uri.as_ref())
                .map(ToOwned::to_owned)
        } else {
            None
        };
        // 2. Let element be the result of creating an element for the token given token, namespace, and the element in
        //    which the adjustedInsertionLocation finds itself.
        let element = self.create_element(
            adjusted_parent,
            namespace_,
            namespace_uri.as_deref(),
            local_name,
            &attributes.0,
            token.had_duplicate_attribute(),
        );
        if local_name == "script" {
            let source_line_number = source_line_number_for_script_token(token);
            if namespace_ == RustFfiHtmlNamespace::Svg {
                self.prepare_svg_script(element, source_line_number);
            } else if namespace_ == RustFfiHtmlNamespace::Html {
                self.set_script_source_line(element, source_line_number);
            }
        }
        drop(attributes);
        // 3. If onlyAddToElementStack is false, then run insert an element at the adjusted insertion location with
        //    element.
        self.insert_parser_created_element(adjusted_parent, adjusted_before, element);
        let template_content = if namespace_ == RustFfiHtmlNamespace::Html && local_name == "template" {
            Some(self.template_content(element))
        } else {
            None
        };
        // 4. Push element onto the stack of open elements so that it is the new current node.
        self.stack_of_open_elements.push(StackNode {
            handle: element,
            local_name: local_name.to_string(),
            namespace_,
            namespace_uri,
            attributes: owned_attributes,
            template_content,
        });
        // 5. Return element.
        element
    }

    fn insert_html_element_for_active_formatting_element(
        &mut self,
        entry: &ActiveFormattingElement,
        parent: usize,
    ) -> usize {
        let (adjusted_parent, adjusted_before) = self.appropriate_place_for_inserting_node(parent);
        let element = self.create_html_element_for_active_formatting_element(entry, adjusted_parent);
        self.insert_parser_created_element(adjusted_parent, adjusted_before, element);
        self.stack_of_open_elements.push(StackNode {
            handle: element,
            local_name: entry.local_name.clone(),
            namespace_: RustFfiHtmlNamespace::Html,
            namespace_uri: None,
            attributes: entry.attributes.clone(),
            template_content: None,
        });
        element
    }

    fn create_html_element_for_active_formatting_element(
        &mut self,
        entry: &ActiveFormattingElement,
        parent: usize,
    ) -> usize {
        self.flush_character_insertions();
        let attributes = attributes_from_owned_attributes(&entry.attributes);
        let element = self.create_element(
            parent,
            RustFfiHtmlNamespace::Html,
            None,
            &entry.local_name,
            &attributes.0,
            false,
        );
        drop(attributes);
        element
    }

    fn create_element(
        &mut self,
        intended_parent: usize,
        namespace_: RustFfiHtmlNamespace,
        namespace_uri: Option<&str>,
        local_name: &str,
        attributes: &[RustFfiHtmlParserAttribute],
        had_duplicate_attribute: bool,
    ) -> usize {
        // https://html.spec.whatwg.org/multipage/parsing.html#create-an-element-for-the-token
        // AD-HOC: DOM node construction stays on the C++ side of LibWeb, so this step crosses the FFI boundary.
        let form_element = self.form_element.unwrap_or(0);
        let has_template_element_on_stack = self.has_template_element_on_stack_of_open_elements();
        unsafe {
            ladybird_html_parser_create_element(
                self.host,
                intended_parent,
                namespace_,
                namespace_uri.map_or(std::ptr::null(), |namespace_uri| namespace_uri.as_ptr()),
                namespace_uri.map_or(0, |namespace_uri| namespace_uri.len()),
                local_name.as_ptr(),
                local_name.len(),
                attributes.as_ptr(),
                attributes.len(),
                had_duplicate_attribute,
                form_element,
                has_template_element_on_stack,
            )
        }
    }

    fn parse_error(&self, message: &str) {
        if !self.log_parse_errors {
            return;
        }
        unsafe { ladybird_html_parser_log_parse_error(self.host, message.as_ptr(), message.len()) }
    }

    fn stop_parsing(&self) {
        unsafe { ladybird_html_parser_stop_parsing(self.host) };
    }

    fn mark_script_already_started(&self, element: usize) {
        unsafe { ladybird_html_parser_mark_script_already_started(self.host, element) };
    }

    fn parse_error_if_stack_contains_unexpected_node_at_end_of_body(&self) {
        for node in &self.stack_of_open_elements {
            if node.namespace_ != RustFfiHtmlNamespace::Html
                || !matches!(
                    node.local_name.as_str(),
                    "dd" | "dt"
                        | "li"
                        | "optgroup"
                        | "option"
                        | "p"
                        | "rb"
                        | "rp"
                        | "rt"
                        | "rtc"
                        | "tbody"
                        | "td"
                        | "tfoot"
                        | "th"
                        | "thead"
                        | "tr"
                        | "body"
                        | "html"
                )
            {
                self.parse_error("unexpected node on stack at end of body");
                break;
            }
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

    fn append_child(&mut self, parent: usize, child: usize) {
        unsafe { ladybird_html_parser_append_child(parent, child) }
    }

    fn insert_node(&mut self, parent: usize, before: usize, child: usize) {
        unsafe { ladybird_html_parser_insert_node(parent, before, child, false) };
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-an-element-at-the-adjusted-insertion-location
    fn insert_parser_created_element(&mut self, parent: usize, before: usize, child: usize) {
        let queue_custom_element_reactions = !self.parsing_fragment;
        unsafe { ladybird_html_parser_insert_node(parent, before, child, queue_custom_element_reactions) };
    }

    fn parent_node(&self, node: usize) -> usize {
        unsafe { ladybird_html_parser_parent_node(node) }
    }

    fn handle_element_popped(&mut self, element: usize) {
        unsafe { ladybird_html_parser_handle_element_popped(element) }
    }

    fn prepare_svg_script(&mut self, element: usize, source_line_number: usize) {
        unsafe { ladybird_html_parser_prepare_svg_script(self.host, element, source_line_number) }
    }

    fn set_script_source_line(&mut self, element: usize, source_line_number: usize) {
        unsafe { ladybird_html_parser_set_script_source_line(self.host, element, source_line_number) }
    }

    fn process_svg_script(&mut self, element: usize) {
        self.pending_svg_script = Some(element);
    }

    fn move_all_children(&mut self, from: usize, to: usize) {
        unsafe { ladybird_html_parser_move_all_children(from, to) }
    }

    fn template_content(&mut self, element: usize) -> usize {
        unsafe { ladybird_html_parser_template_content(element) }
    }

    fn attach_declarative_shadow_root(&mut self, init: DeclarativeShadowRootInit) -> usize {
        unsafe {
            ladybird_html_parser_attach_declarative_shadow_root(
                init.host,
                init.mode,
                init.slot_assignment,
                init.clonable,
                init.serializable,
                init.delegates_focus,
                init.keep_custom_element_registry_null,
            )
        }
    }

    fn set_template_content(&mut self, element: usize, content: usize) {
        unsafe { ladybird_html_parser_set_template_content(element, content) }
    }

    fn allows_declarative_shadow_roots(&self, node: usize) -> bool {
        unsafe { ladybird_html_parser_allows_declarative_shadow_roots(node) }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead:attr-template-shadowrootmode
    fn try_to_start_declarative_shadow_root(&mut self, token: &Token) -> bool {
        // 6. Let the adjustedInsertionLocation be the appropriate place for inserting a node.
        let (adjusted_insertion_location_parent, adjusted_insertion_location_before) =
            self.appropriate_place_for_inserting_node(self.current_node_handle());

        // 7. Let intendedParent be the element in which the adjustedInsertionLocation finds itself.
        let intended_parent = adjusted_insertion_location_parent;

        // 8. Let document be intendedParent's node document.

        // 9. If any of the following are false:
        //    - templateStartTag's shadowrootmode is not in the None state;
        let mode = match token.attribute("shadowrootmode") {
            Some(value) if value.eq_ignore_ascii_case("open") => RustFfiHtmlShadowRootMode::Open,
            Some(value) if value.eq_ignore_ascii_case("closed") => RustFfiHtmlShadowRootMode::Closed,
            _ => return false,
        };

        //    - document's allow declarative shadow roots is true; or
        if !self.allows_declarative_shadow_roots(intended_parent) {
            return false;
        }

        //    - the adjusted current node is not the topmost element in the stack of open elements,
        let Some(adjusted_current_node) = self.adjusted_current_node() else {
            return false;
        };
        let topmost_element = self.stack_of_open_elements.first().map(|node| node.handle);
        if topmost_element == Some(adjusted_current_node.handle) {
            return false;
        }

        // 10. Otherwise:

        // 1. Let declarativeShadowHostElement be adjusted current node.
        let declarative_shadow_host_element = adjusted_current_node.handle;
        if declarative_shadow_host_element == 0 {
            return false;
        };

        // 2. Let template be the result of insert a foreign element for templateStartTag, with HTML namespace and true.
        let attributes = attributes_from_token(token, RustFfiHtmlNamespace::Html);
        let owned_attributes = owned_attributes_from_token(token, RustFfiHtmlNamespace::Html);
        let template = self.create_element(
            intended_parent,
            RustFfiHtmlNamespace::Html,
            None,
            "template",
            &attributes.0,
            token.had_duplicate_attribute(),
        );
        drop(attributes);
        let template_content = self.template_content(template);
        self.stack_of_open_elements.push(StackNode {
            handle: template,
            local_name: "template".to_string(),
            namespace_: RustFfiHtmlNamespace::Html,
            namespace_uri: None,
            attributes: owned_attributes,
            template_content: Some(template_content),
        });

        // 3. Let mode be templateStartTag's shadowrootmode attribute's value.

        // 4. Let slotAssignment be "named".
        let mut slot_assignment = RustFfiHtmlSlotAssignmentMode::Named;

        // 5. If templateStartTag's shadowrootslotassignment attribute is in the Manual state, then set slotAssignment to "manual".
        if token
            .attribute("shadowrootslotassignment")
            .is_some_and(|value| value.eq_ignore_ascii_case("manual"))
        {
            slot_assignment = RustFfiHtmlSlotAssignmentMode::Manual;
        }

        // 6. Let clonable be true if templateStartTag has a shadowrootclonable attribute; otherwise false.
        let clonable = token.has_attribute("shadowrootclonable");

        // 7. Let serializable be true if templateStartTag has a shadowrootserializable attribute; otherwise false.
        let serializable = token.has_attribute("shadowrootserializable");

        // 8. Let delegatesFocus be true if templateStartTag has a shadowrootdelegatesfocus attribute; otherwise false.
        let delegates_focus = token.has_attribute("shadowrootdelegatesfocus");

        // 9. If declarativeShadowHostElement is a shadow host, then insert an element at the adjusted insertion location with template.
        //
        // This is handled by the host attach hook returning 0 below.

        // 10. Otherwise:

        // 1. Let registry be null if templateStartTag has a shadowrootcustomelementregistry attribute;
        //    otherwise declarativeShadowHostElement's node document's custom element registry.

        // 2. Attach a shadow root with declarativeShadowHostElement, mode, clonable, serializable,
        //    delegatesFocus, slotAssignment, and registry.
        let shadow_root = self.attach_declarative_shadow_root(DeclarativeShadowRootInit {
            host: declarative_shadow_host_element,
            mode,
            slot_assignment,
            clonable,
            serializable,
            delegates_focus,
            keep_custom_element_registry_null: token.has_attribute("shadowrootcustomelementregistry"),
        });

        // If an exception is thrown, then catch it and:
        if shadow_root == 0 {
            // 1. Insert an element at the adjusted insertion location with template.
            self.insert_node(
                adjusted_insertion_location_parent,
                adjusted_insertion_location_before,
                template,
            );
            // 2. The user agent may report an error to the developer console.
            // 3. Return.
            return true;
        }

        // 3. Let shadow be declarativeShadowHostElement's shadow root.

        // 4. Set shadow's declarative to true.

        // 5. Set template's template contents to shadow.
        self.set_template_content(template, shadow_root);
        self.stack_of_open_elements.last_mut().unwrap().template_content = Some(shadow_root);

        // 6. Set shadow's available to element internals to true.

        // 7. If templateStartTag has a shadowrootcustomelementregistry attribute, then set shadow's keep
        //    custom element registry null to true.
        true
    }

    fn handle_template_start_tag(&mut self, token: &Token) {
        // 2. Insert a marker at the end of the list of active formatting elements.
        self.insert_marker_at_the_end_of_the_list_of_active_formatting_elements();
        // 3. Set the frameset-ok flag to "not ok".
        self.frameset_ok = false;
        // 4. Switch the insertion mode to "in template".
        self.insertion_mode = InsertionMode::InTemplate;
        // 5. Push "in template" onto the stack of template insertion modes so that it is the new current template insertion mode.
        self.stack_of_template_insertion_modes.push(InsertionMode::InTemplate);

        if self.try_to_start_declarative_shadow_root(token) {
            return;
        }

        self.insert_html_element_for(token, self.current_insertion_parent_handle());
    }

    fn handle_template_end_tag(&mut self) {
        // If there is no template element on the stack of open elements, then this is a parse error; ignore the token.
        if !self.has_template_element_on_stack_of_open_elements() {
            self.parse_error("template end tag without template element on stack");
            return;
        }

        // Otherwise, run these steps:

        // 1. Generate all implied end tags thoroughly.
        self.generate_all_implied_end_tags_thoroughly();

        // 2. If the current node is not a template element, then this is a parse error.
        if !self.current_node_named("template") {
            self.parse_error("template end tag with different current node");
        }

        // 3. Pop elements from the stack of open elements until a template element has been popped from the stack.
        self.pop_until_tag_name_has_been_popped("template");
        // 4. Clear the list of active formatting elements up to the last marker.
        self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
        // 5. Pop the current template insertion mode off the stack of template insertion modes.
        self.stack_of_template_insertion_modes.pop();
        // 6. Reset the insertion mode appropriately.
        self.reset_the_insertion_mode_appropriately();
    }

    fn set_document_quirks_mode(&mut self, mode: RustFfiHtmlQuirksMode) {
        self.document_quirks_mode = mode;
        unsafe { ladybird_html_parser_set_document_quirks_mode(self.host, mode) }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-a-comment
    fn insert_comment(&mut self, data: &str) {
        let parent = self.current_insertion_parent_handle();
        self.append_comment_to_node(parent, data);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-a-comment
    fn append_comment_to_node(&mut self, parent: usize, data: &str) {
        self.flush_character_insertions();
        let comment = self.create_comment(data);
        self.append_child(parent, comment);
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-a-character
    fn insert_character(&mut self, code_point: u32) {
        if let Some(character) = char::from_u32(code_point) {
            self.pending_text.push(character);
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#insert-a-character
    fn flush_character_insertions(&mut self) {
        if self.pending_text.is_empty() {
            return;
        }
        // AD-HOC: Coalesce consecutive character tokens before asking the host to insert text. The host still runs the
        // same DOM insertion logic for the adjusted insertion location.
        let data = std::mem::take(&mut self.pending_text);
        let (parent, before) = self.appropriate_place_for_inserting_node(self.current_node_handle());
        self.insert_text(parent, before, &data);
    }

    fn insert_text(&mut self, parent: usize, before: usize, data: &str) {
        unsafe { ladybird_html_parser_insert_text(parent, before, data.as_ptr(), data.len()) }
    }

    fn add_missing_attributes_to_element(&mut self, element: usize, token: &Token) {
        let TokenPayload::Tag { attributes, .. } = &token.payload else {
            return;
        };
        for attribute in attributes {
            let local_name = if attribute.local_name_id != 0 {
                interned_names::attr_name_by_id(attribute.local_name_id).unwrap_or_default()
            } else {
                attribute.local_name.as_bytes()
            };
            unsafe {
                ladybird_html_parser_add_missing_attribute(
                    element,
                    local_name.as_ptr(),
                    local_name.len(),
                    attribute.value.as_ptr(),
                    attribute.value.len(),
                );
            }
        }
    }

    fn pop_current_node(&mut self) {
        self.flush_character_insertions();
        self.pop_stack_node();
    }

    fn pop_stack_node(&mut self) -> Option<StackNode> {
        if self.stack_of_open_elements.len() <= 1 {
            return None;
        }
        let node = self.stack_of_open_elements.pop();
        if let Some(node) = node.as_ref() {
            // https://html.spec.whatwg.org/multipage/parsing.html#stack-of-open-elements
            // When the current node is removed from the stack of open elements, process internal resource links.
            //
            // AD-HOC: Callers flush buffered text before popping so option selectedcontent cloning sees up-to-date text.
            self.handle_element_popped(node.handle);
        }
        node
    }

    fn pop_until_tag_name_has_been_popped(&mut self, tag_name: &str) {
        self.flush_character_insertions();
        while self.stack_of_open_elements.len() > 1 {
            let node = self.pop_stack_node().unwrap();
            if node.local_name == tag_name && node.namespace_ == RustFfiHtmlNamespace::Html {
                break;
            }
        }
    }

    fn pop_until_one_of_tag_names_has_been_popped(&mut self, tag_names: &[&str]) {
        self.flush_character_insertions();
        while self.stack_of_open_elements.len() > 1 {
            let node = self.pop_stack_node().unwrap();
            if node.namespace_ == RustFfiHtmlNamespace::Html && tag_names.contains(&node.local_name.as_str()) {
                break;
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#generate-implied-end-tags
    fn generate_implied_end_tags(&mut self) {
        self.generate_implied_end_tags_except("");
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#generate-implied-end-tags
    fn generate_implied_end_tags_except(&mut self, exception: &str) {
        self.flush_character_insertions();
        while self.stack_of_open_elements.len() > 1
            && self
                .stack_of_open_elements
                .last()
                .is_some_and(|node| node.local_name != exception && is_implied_end_tag(node.local_name.as_str()))
        {
            self.pop_stack_node();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#generate-implied-end-tags
    fn generate_all_implied_end_tags_thoroughly(&mut self) {
        self.flush_character_insertions();
        while self
            .stack_of_open_elements
            .last()
            .is_some_and(|node| is_thoroughly_implied_end_tag(node.local_name.as_str()))
        {
            self.pop_stack_node();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#clear-the-stack-back-to-a-table-context
    fn clear_the_stack_back_to_a_table_context(&mut self) {
        self.flush_character_insertions();
        // While the current node is not a table, template, or html element, pop elements from the stack of open
        // elements.
        while self.stack_of_open_elements.len() > 1
            && !self.current_node_named("table")
            && !self.current_node_named("template")
            && !self.current_node_named("html")
        {
            self.pop_stack_node();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#clear-the-stack-back-to-a-table-body-context
    fn clear_the_stack_back_to_a_table_body_context(&mut self) {
        self.flush_character_insertions();
        // While the current node is not a tbody, tfoot, thead, template, or html element, pop elements from the stack
        // of open elements.
        while self.stack_of_open_elements.len() > 1
            && !self.current_node_named("tbody")
            && !self.current_node_named("tfoot")
            && !self.current_node_named("thead")
            && !self.current_node_named("template")
            && !self.current_node_named("html")
        {
            self.pop_stack_node();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#clear-the-stack-back-to-a-table-row-context
    fn clear_the_stack_back_to_a_table_row_context(&mut self) {
        self.flush_character_insertions();
        // While the current node is not a tr, template, or html element, pop elements from the stack of open elements.
        while self.stack_of_open_elements.len() > 1
            && !self.current_node_named("tr")
            && !self.current_node_named("template")
            && !self.current_node_named("html")
        {
            self.pop_stack_node();
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-scope
    fn has_heading_in_scope(&self) -> bool {
        ["h1", "h2", "h3", "h4", "h5", "h6"]
            .iter()
            .any(|tag_name| self.has_in_scope(tag_name))
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-button-scope
    fn has_in_button_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if is_scope_boundary(node) || (node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == "button")
            {
                return false;
            }
        }
        false
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-list-item-scope
    fn has_in_list_item_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if is_scope_boundary(node)
                || (node.namespace_ == RustFfiHtmlNamespace::Html && matches!(node.local_name.as_str(), "ol" | "ul"))
            {
                return false;
            }
        }
        false
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-scope
    fn has_in_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if is_scope_boundary(node) {
                return false;
            }
        }
        false
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-scope
    fn has_in_scope_by_handle(&self, handle: usize) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.handle == handle {
                return true;
            }
            if is_scope_boundary(node) {
                return false;
            }
        }
        false
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-table-scope
    fn has_in_table_scope(&self, tag_name: &str) -> bool {
        for node in self.stack_of_open_elements.iter().rev() {
            if node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name {
                return true;
            }
            if node.namespace_ == RustFfiHtmlNamespace::Html
                && matches!(node.local_name.as_str(), "html" | "table" | "template")
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

    fn has_on_stack_of_open_elements(&self, tag_name: &str) -> bool {
        self.stack_of_open_elements
            .iter()
            .any(|node| node.namespace_ == RustFfiHtmlNamespace::Html && node.local_name == tag_name)
    }

    fn has_template_element_on_stack_of_open_elements(&self) -> bool {
        self.has_on_stack_of_open_elements("template")
    }

    fn pop_until_heading_has_been_popped(&mut self) {
        self.flush_character_insertions();
        while self.stack_of_open_elements.len() > 1 {
            let node = self.pop_stack_node().unwrap();
            if node.namespace_ == RustFfiHtmlNamespace::Html
                && matches!(node.local_name.as_str(), "h1" | "h2" | "h3" | "h4" | "h5" | "h6")
            {
                break;
            }
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#close-a-p-element
    fn close_a_p_element(&mut self) {
        // 1. Generate implied end tags, except for p elements.
        self.generate_implied_end_tags_except("p");
        // 2. If the current node is not a p element, then this is a parse error.
        if !self.current_node_named("p") {
            self.parse_error("current node is not p element while closing p element");
        }
        // 3. Pop elements from the stack of open elements until a p element has been popped from the stack.
        self.pop_until_tag_name_has_been_popped("p");
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#close-the-cell
    fn close_the_cell(&mut self) {
        // 1. Generate implied end tags.
        self.generate_implied_end_tags();
        // 2. If the current node is not now a td element or a th element, then this is a parse error.
        if !self.current_node_is_one_of(&["td", "th"]) {
            self.parse_error("current node is not a cell element while closing cell");
        }
        // 3. Pop elements from the stack of open elements until a td element or a th element has been popped from the
        //    stack.
        self.pop_until_one_of_tag_names_has_been_popped(&["td", "th"]);
        // 4. Clear the list of active formatting elements up to the last marker.
        self.clear_the_list_of_active_formatting_elements_up_to_the_last_marker();
        // 5. Switch the insertion mode to "in row".
        self.insertion_mode = InsertionMode::InRow;
    }

    fn foster_parenting_location(&self) -> (usize, usize) {
        if let Some(table_index) = self
            .stack_of_open_elements
            .iter()
            .rposition(|node| node.local_name == "table" && node.namespace_ == RustFfiHtmlNamespace::Html)
            && table_index > 0
        {
            let table = self.stack_of_open_elements[table_index].handle;
            let parent = self.parent_node(table);
            if parent != 0 {
                return (parent, table);
            }

            let parent_node = &self.stack_of_open_elements[table_index - 1];
            let parent = parent_node.template_content.unwrap_or(parent_node.handle);
            return (parent, 0);
        }
        (
            self.stack_of_open_elements.first().map(|node| node.handle).unwrap_or(0),
            0,
        )
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#appropriate-place-for-inserting-a-node
    fn appropriate_place_for_inserting_node(&self, target: usize) -> (usize, usize) {
        let Some(target_node) = self.stack_of_open_elements.iter().find(|node| node.handle == target) else {
            return (target, 0);
        };

        if !self.foster_parenting_enabled
            || target_node.namespace_ != RustFfiHtmlNamespace::Html
            || !matches!(
                target_node.local_name.as_str(),
                "table" | "tbody" | "tfoot" | "thead" | "tr"
            )
        {
            return (target_node.template_content.unwrap_or(target_node.handle), 0);
        }

        let last_template_index = self
            .stack_of_open_elements
            .iter()
            .rposition(|node| node.local_name == "template" && node.namespace_ == RustFfiHtmlNamespace::Html);
        let last_table_index = self
            .stack_of_open_elements
            .iter()
            .rposition(|node| node.local_name == "table" && node.namespace_ == RustFfiHtmlNamespace::Html);

        if let Some(template_index) = last_template_index
            && last_table_index.is_none_or(|table_index| template_index > table_index)
        {
            let template = &self.stack_of_open_elements[template_index];
            return (template.template_content.unwrap_or(template.handle), 0);
        }

        self.foster_parenting_location()
    }

    fn insert_marker_at_the_end_of_the_list_of_active_formatting_elements(&mut self) {
        self.list_of_active_formatting_elements.push(ActiveFormattingElement {
            handle: 0,
            local_name: String::new(),
            attributes: Vec::new(),
            is_marker: true,
        });
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#push-onto-the-list-of-active-formatting-elements
    fn push_onto_the_list_of_active_formatting_elements(&mut self, element: usize, token: &Token) {
        let attributes = owned_attributes_from_token(token, RustFfiHtmlNamespace::Html);

        // 1. If there are already three elements in the list of active formatting elements after the last marker, if any, or anywhere in the list if there are no markers,
        //    that have the same tag name, namespace, and attributes as element, then remove the earliest such element from the list of active formatting elements.
        //    For these purposes, the attributes must be compared as they were when the elements were created by the parser; two elements have the same attributes if all their parsed attributes
        //    can be paired such that the two attributes in each pair have identical names, namespaces, and values (the order of the attributes does not matter).
        let first_index_after_last_marker = self.active_formatting_elements_after_last_marker_start_index();
        let matching_entries = self.list_of_active_formatting_elements[first_index_after_last_marker..]
            .iter()
            .filter(|entry| {
                !entry.is_marker
                    && entry.local_name == token.tag_name()
                    && active_formatting_element_has_same_attributes(entry, &attributes)
            })
            .count();
        if matching_entries >= 3
            && let Some(index) = self.list_of_active_formatting_elements[first_index_after_last_marker..]
                .iter()
                .position(|entry| {
                    !entry.is_marker
                        && entry.local_name == token.tag_name()
                        && active_formatting_element_has_same_attributes(entry, &attributes)
                })
                .map(|index| first_index_after_last_marker + index)
        {
            self.list_of_active_formatting_elements.remove(index);
        }

        // 2. Add element to the list of active formatting elements.
        self.list_of_active_formatting_elements.push(ActiveFormattingElement {
            handle: element,
            local_name: token.tag_name().to_string(),
            attributes,
            is_marker: false,
        });
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#clear-the-list-of-active-formatting-elements-up-to-the-last-marker
    fn clear_the_list_of_active_formatting_elements_up_to_the_last_marker(&mut self) {
        // 1. Let entry be the last (most recently added) entry in the list of active formatting elements.
        while let Some(entry) = self.list_of_active_formatting_elements.pop() {
            // 2. Remove entry from the list of active formatting elements.
            if entry.is_marker {
                // 3. If entry was a marker, then stop the algorithm at this point.
                break;
            }
            // 4. Go to step 1.
        }
    }

    fn active_formatting_elements_after_last_marker_start_index(&self) -> usize {
        self.list_of_active_formatting_elements
            .iter()
            .rposition(|entry| entry.is_marker)
            .map_or(0, |index| index + 1)
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#reconstruct-the-active-formatting-elements
    fn reconstruct_the_active_formatting_elements(&mut self) {
        let mut first_missing_index = None;
        for (index, entry) in self.list_of_active_formatting_elements.iter().enumerate().rev() {
            if entry.is_marker {
                break;
            }
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
            if entry.is_marker {
                continue;
            }
            let element =
                self.insert_html_element_for_active_formatting_element(&entry, self.current_insertion_parent_handle());
            self.list_of_active_formatting_elements[index].handle = element;
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#adoptionAgency
    fn run_the_adoption_agency_algorithm(&mut self, tag_name: &str) -> AdoptionAgencyAlgorithmOutcome {
        // 1. Let subject be token's tag name.
        let subject = tag_name;

        // 2. If the current node is an HTML element whose tag name is subject,
        //    and the current node is not in the list of active formatting elements,
        //    then pop the current node off the stack of open elements, and return.
        if self.current_node_named(subject) {
            let current_node = self.current_node_handle();
            if !self
                .list_of_active_formatting_elements
                .iter()
                .any(|entry| entry.handle == current_node)
            {
                self.pop_current_node();
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            }
        }

        // 3. Let outerLoopCounter be 0.
        let mut outer_loop_counter: u8 = 0;

        // 4. While true:
        loop {
            // 1. If outerLoopCounter is greater than or equal to 8, then return.
            if outer_loop_counter >= 8 {
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            }

            // 2. Increment outerLoopCounter by 1.
            outer_loop_counter += 1;

            // 3. Let formattingElement be the last element in the list of active formatting elements that:
            //    - is between the end of the list and the last marker in the list, if any, or the start of the list otherwise, and
            //    - has the tag name subject.
            let active_formatting_elements_after_last_marker =
                self.active_formatting_elements_after_last_marker_start_index();
            let Some(formatting_index) = self
                .list_of_active_formatting_elements
                .iter()
                .enumerate()
                .skip(active_formatting_elements_after_last_marker)
                .rposition(|(_, entry)| entry.local_name == subject)
                .map(|index| active_formatting_elements_after_last_marker + index)
            else {
                //    If there is no such element, then return and instead act as described in the "any other end tag" entry above.
                return AdoptionAgencyAlgorithmOutcome::RunAnyOtherEndTagSteps;
            };
            let formatting_element = self.list_of_active_formatting_elements[formatting_index].clone();

            // 4. If formattingElement is not in the stack of open elements,
            let Some(stack_index) = self
                .stack_of_open_elements
                .iter()
                .position(|node| node.handle == formatting_element.handle)
            else {
                // then this is a parse error;
                self.parse_error("adoption agency formatting element is not on stack of open elements");
                // remove the element from the list,
                self.list_of_active_formatting_elements.remove(formatting_index);
                // and return.
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            };

            // 5. If formattingElement is in the stack of open elements, but the element is not in scope,
            if !self.has_in_scope_by_handle(formatting_element.handle) {
                // then this is a parse error;
                self.parse_error("adoption agency formatting element is not in scope");
                // return.
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            }

            // 6. If formattingElement is not the current node,
            // this is a parse error. (But do not return.)
            if self.current_node_handle() != formatting_element.handle {
                self.parse_error("adoption agency formatting element is not the current node");
            }

            // 7. Let furthestBlock be the topmost node in the stack of open elements that is lower in the stack than formattingElement,
            //    and is an element in the special category. There might not be one.
            let Some(furthest_block_index) = self.stack_of_open_elements[stack_index + 1..]
                .iter()
                .position(is_special_tag)
                .map(|index| stack_index + 1 + index)
            else {
                // 8. If there is no furthestBlock,
                // then the UA must first pop all the nodes from the bottom of the stack of open elements,
                // from the current node up to and including formattingElement,
                while self.current_node_handle() != formatting_element.handle {
                    self.pop_current_node();
                }
                self.pop_current_node();

                // then remove formattingElement from the list of active formatting elements,
                self.remove_active_formatting_element(formatting_element.handle);
                // and finally return.
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            };
            let furthest_block = self.stack_of_open_elements[furthest_block_index].clone();

            // 9. Let commonAncestor be the element immediately above formattingElement in the stack of open elements.
            if stack_index == 0 {
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            }
            let common_ancestor = self.stack_of_open_elements[stack_index - 1].handle;

            // 10. Let a bookmark note the position of formattingElement in the list of active formatting elements
            //     relative to the elements on either side of it in the list.
            let mut bookmark = formatting_index;

            // 11. Let node and lastNode be furthestBlock.
            let mut node_index = furthest_block_index;
            let mut last_node = furthest_block.handle;

            // 12. Let innerLoopCounter be 0.
            let mut inner_loop_counter: u8 = 0;

            // 13. While true:
            loop {
                // 1. Increment innerLoopCounter by 1.
                inner_loop_counter += 1;

                // 2. Let node be the element immediately above node in the stack of open elements,
                //    or if node is no longer in the stack of open elements (e.g. because it got removed by this algorithm),
                //    the element that was immediately above node in the stack of open elements before node was removed.
                if node_index == 0 {
                    return AdoptionAgencyAlgorithmOutcome::DoNothing;
                }
                node_index -= 1;
                let node = self.stack_of_open_elements[node_index].clone();

                // 3. If node is formattingElement, then break.
                if node.handle == formatting_element.handle {
                    break;
                }

                // 4. If innerLoopCounter is greater than 3 and node is in the list of active formatting elements,
                let mut active_index = self
                    .list_of_active_formatting_elements
                    .iter()
                    .position(|entry| entry.handle == node.handle);
                if inner_loop_counter > 3
                    && let Some(index) = active_index
                {
                    if index < bookmark {
                        bookmark -= 1;
                    }
                    // then remove node from the list of active formatting elements.
                    self.list_of_active_formatting_elements.remove(index);
                    active_index = None;
                }

                // 5. If node is not in the list of active formatting elements,
                let Some(active_index) = active_index else {
                    // then remove node from the stack of open elements and continue.
                    self.stack_of_open_elements.remove(node_index);
                    continue;
                };

                // 6. Create an element for the token for which the element node was created,
                //    in the HTML namespace, with commonAncestor as the intended parent;
                let entry = self.list_of_active_formatting_elements[active_index].clone();
                let new_element = self.create_html_element_for_active_formatting_element(&entry, common_ancestor);
                //    replace the entry for node in the list of active formatting elements with an entry for the new element,
                self.list_of_active_formatting_elements[active_index].handle = new_element;
                //    replace the entry for node in the stack of open elements with an entry for the new element,
                self.stack_of_open_elements[node_index] = StackNode {
                    handle: new_element,
                    local_name: entry.local_name,
                    namespace_: RustFfiHtmlNamespace::Html,
                    namespace_uri: None,
                    attributes: entry.attributes,
                    template_content: None,
                };

                // 7. If lastNode is furthestBlock,
                if last_node == furthest_block.handle {
                    // then move the aforementioned bookmark to be immediately after the new node in the list of active formatting elements.
                    bookmark = active_index + 1;
                }

                // 8. Append lastNode to node.
                self.append_child(new_element, last_node);

                // 9. Set lastNode to node.
                last_node = new_element;
            }

            // 14. Insert whatever lastNode ended up being in the previous step at the appropriate place for inserting a node,
            //     but using commonAncestor as the override target.
            let (parent, before) = self.appropriate_place_for_inserting_node(common_ancestor);
            self.insert_node(parent, before, last_node);

            // 15. Create an element for the token for which formattingElement was created,
            //     in the HTML namespace, with furthestBlock as the intended parent.
            let Some(formatting_element_index) = self
                .list_of_active_formatting_elements
                .iter()
                .position(|entry| entry.handle == formatting_element.handle)
            else {
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            };
            let entry = self.list_of_active_formatting_elements[formatting_element_index].clone();
            let new_element = self.create_html_element_for_active_formatting_element(&entry, furthest_block.handle);

            // 16. Take all of the child nodes of furthestBlock and append them to the element created in the last step.
            self.move_all_children(furthest_block.handle, new_element);

            // 17. Append that new element to furthestBlock.
            self.append_child(furthest_block.handle, new_element);

            // 18. Remove formattingElement from the list of active formatting elements,
            //     and insert the new element into the list of active formatting elements at the position of the aforementioned bookmark.
            if formatting_element_index < bookmark {
                bookmark -= 1;
            }
            self.list_of_active_formatting_elements.remove(formatting_element_index);
            let active_formatting_insertion_index = bookmark.min(self.list_of_active_formatting_elements.len());
            self.list_of_active_formatting_elements.insert(
                active_formatting_insertion_index,
                ActiveFormattingElement {
                    handle: new_element,
                    local_name: entry.local_name.clone(),
                    attributes: entry.attributes.clone(),
                    is_marker: false,
                },
            );

            // 19. Remove formattingElement from the stack of open elements, and insert the new element
            //     into the stack of open elements immediately below the position of furthestBlock in that stack.
            if let Some(formatting_element_stack_index) = self
                .stack_of_open_elements
                .iter()
                .position(|node| node.handle == formatting_element.handle)
            {
                self.stack_of_open_elements.remove(formatting_element_stack_index);
            }
            let Some(furthest_block_stack_index) = self
                .stack_of_open_elements
                .iter()
                .position(|node| node.handle == furthest_block.handle)
            else {
                return AdoptionAgencyAlgorithmOutcome::DoNothing;
            };
            self.stack_of_open_elements.insert(
                furthest_block_stack_index + 1,
                StackNode {
                    handle: new_element,
                    local_name: entry.local_name,
                    namespace_: RustFfiHtmlNamespace::Html,
                    namespace_uri: None,
                    attributes: entry.attributes,
                    template_content: None,
                },
            );
        }
    }

    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inbody
    fn process_any_other_end_tag(&mut self, tag_name: &str) {
        // Run these steps:
        for index in (0..self.stack_of_open_elements.len()).rev() {
            let node = &self.stack_of_open_elements[index];
            // 1. Initialize node to be the current node (the bottommost node of the stack).
            // 2. If node is an HTML element with the same tag name as the token, then:
            if node.local_name == tag_name {
                let node_handle = node.handle;
                // 1. Generate implied end tags, except for HTML elements with the same tag name as the token.
                self.generate_implied_end_tags_except(tag_name);
                // 2. If node is not the current node, then this is a parse error.
                if self.current_node_handle() != node_handle {
                    self.parse_error("matching node is not current node for any other end tag");
                }
                // 3. Pop all the nodes from the current node up to node, including node.
                while self
                    .stack_of_open_elements
                    .last()
                    .is_some_and(|current_node| current_node.handle != node_handle)
                {
                    self.pop_stack_node();
                }
                if self
                    .stack_of_open_elements
                    .last()
                    .is_some_and(|current_node| current_node.handle == node_handle)
                {
                    self.pop_stack_node();
                }
                return;
            }

            // 3. Otherwise, if node is in the special category, then this is a parse error; return.
            if is_special_tag(node) {
                self.parse_error("special element reached while processing any other end tag");
                return;
            }
            // 4. Set node to the previous entry in the stack of open elements.
        }
    }

    fn remove_active_formatting_element(&mut self, handle: usize) {
        self.list_of_active_formatting_elements
            .retain(|entry| entry.handle != handle);
    }
}

struct AttributeStorage {
    local_name_bytes: Vec<Vec<u8>>,
    prefix_bytes: Vec<Vec<u8>>,
    value_bytes: Vec<Vec<u8>>,
}

struct AdjustedAttributeName {
    local_name: String,
    prefix: Option<&'static str>,
    namespace_: RustFfiHtmlAttributeNamespace,
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
        let adjusted_name = adjusted_foreign_attribute_name(&local_name, namespace_);
        attributes.push(OwnedAttribute {
            local_name: adjusted_name.local_name,
            prefix: adjusted_name.prefix.map(ToOwned::to_owned),
            namespace_: adjusted_name.namespace_,
            value: attribute.value.clone(),
        });
    }
    attributes
}

unsafe fn string_from_ffi(ptr: *const u8, len: usize) -> String {
    if ptr.is_null() || len == 0 {
        return String::new();
    }

    let bytes = unsafe { std::slice::from_raw_parts(ptr, len) };
    String::from_utf8_lossy(bytes).to_string()
}

unsafe fn namespace_uri_from_ffi(namespace_: RustFfiHtmlNamespace, ptr: *const u8, len: usize) -> Option<String> {
    if namespace_ != RustFfiHtmlNamespace::Other || ptr.is_null() || len == 0 {
        return None;
    }
    Some(unsafe { string_from_ffi(ptr, len) })
}

unsafe fn owned_attributes_from_ffi(
    attributes: *const RustFfiHtmlParserAttribute,
    attribute_count: usize,
) -> Vec<OwnedAttribute> {
    if attributes.is_null() || attribute_count == 0 {
        return Vec::new();
    }

    let attributes = unsafe { std::slice::from_raw_parts(attributes, attribute_count) };
    attributes
        .iter()
        .map(|attribute| OwnedAttribute {
            local_name: unsafe { string_from_ffi(attribute.local_name_ptr, attribute.local_name_len) },
            prefix: if attribute.prefix_len == 0 {
                None
            } else {
                Some(unsafe { string_from_ffi(attribute.prefix_ptr, attribute.prefix_len) })
            },
            namespace_: attribute.namespace_,
            value: unsafe { string_from_ffi(attribute.value_ptr, attribute.value_len) },
        })
        .collect()
}

fn active_formatting_element_has_same_attributes(
    active_formatting_element: &ActiveFormattingElement,
    attributes: &[OwnedAttribute],
) -> bool {
    active_formatting_element.attributes.len() == attributes.len()
        && active_formatting_element.attributes.iter().all(|attribute| {
            attributes.iter().any(|other_attribute| {
                attribute.local_name == other_attribute.local_name
                    && attribute.namespace_ == other_attribute.namespace_
                    && attribute.value == other_attribute.value
            })
        })
}

fn source_line_number_for_script_token(token: &Token) -> usize {
    token.end_position.line.saturating_add(1).min(usize::MAX as u64) as usize
}

fn attributes_from_owned_attributes(
    attributes: &[OwnedAttribute],
) -> (Vec<RustFfiHtmlParserAttribute>, AttributeStorage) {
    let mut storage = AttributeStorage {
        local_name_bytes: Vec::with_capacity(attributes.len()),
        prefix_bytes: Vec::new(),
        value_bytes: Vec::with_capacity(attributes.len()),
    };
    let mut ffi_attributes = Vec::with_capacity(attributes.len());

    for attribute in attributes {
        storage.local_name_bytes.push(attribute.local_name.as_bytes().to_vec());
        storage.value_bytes.push(attribute.value.as_bytes().to_vec());

        let local_name_bytes = storage.local_name_bytes.last().unwrap();
        let (prefix_ptr, prefix_len) = match &attribute.prefix {
            Some(prefix) => {
                storage.prefix_bytes.push(prefix.as_bytes().to_vec());
                let prefix_bytes = storage.prefix_bytes.last().unwrap();
                (prefix_bytes.as_ptr(), prefix_bytes.len())
            }
            None => (std::ptr::null(), 0),
        };
        let value_bytes = storage.value_bytes.last().unwrap();
        ffi_attributes.push(RustFfiHtmlParserAttribute {
            local_name_ptr: local_name_bytes.as_ptr(),
            local_name_len: local_name_bytes.len(),
            prefix_ptr,
            prefix_len,
            namespace_: attribute.namespace_,
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
        prefix_bytes: Vec::new(),
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
        let adjusted_name = adjusted_foreign_attribute_name(&local_name, namespace_);
        storage
            .local_name_bytes
            .push(adjusted_name.local_name.as_bytes().to_vec());
        storage.value_bytes.push(attribute.value.as_bytes().to_vec());

        let local_name_bytes = storage.local_name_bytes.last().unwrap();
        let (prefix_ptr, prefix_len) = match adjusted_name.prefix {
            Some(prefix) => {
                storage.prefix_bytes.push(prefix.as_bytes().to_vec());
                let prefix_bytes = storage.prefix_bytes.last().unwrap();
                (prefix_bytes.as_ptr(), prefix_bytes.len())
            }
            None => (std::ptr::null(), 0),
        };
        let value_bytes = storage.value_bytes.last().unwrap();
        attributes.push(RustFfiHtmlParserAttribute {
            local_name_ptr: local_name_bytes.as_ptr(),
            local_name_len: local_name_bytes.len(),
            prefix_ptr,
            prefix_len,
            namespace_: adjusted_name.namespace_,
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
    // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inforeign
    // If the adjusted current node is an element in the SVG namespace, and the token's tag name is one of the ones in
    // the first column of the following table, change the tag name to the name given in the corresponding cell in the
    // second column.
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

fn adjusted_foreign_attribute_name(attribute_name: &[u8], namespace_: RustFfiHtmlNamespace) -> AdjustedAttributeName {
    let Ok(attribute_name) = std::str::from_utf8(attribute_name) else {
        return AdjustedAttributeName {
            local_name: String::new(),
            prefix: None,
            namespace_: RustFfiHtmlAttributeNamespace::None,
        };
    };

    if namespace_ == RustFfiHtmlNamespace::Html {
        return AdjustedAttributeName {
            local_name: attribute_name.to_string(),
            prefix: None,
            namespace_: RustFfiHtmlAttributeNamespace::None,
        };
    }

    let adjusted_name = match namespace_ {
        // https://html.spec.whatwg.org/multipage/parsing.html#adjust-mathml-attributes
        // When the steps below require the user agent to adjust MathML attributes for a token, then, if the token has
        // an attribute named definitionurl, change its name to definitionURL.
        RustFfiHtmlNamespace::MathMl => match attribute_name {
            "definitionurl" => "definitionURL",
            _ => attribute_name,
        },
        // https://html.spec.whatwg.org/multipage/parsing.html#adjust-svg-attributes
        // When the steps below require the user agent to adjust SVG attributes for a token, then, for each attribute
        // on the token whose attribute name is one of the ones in the first column of the following table, change the
        // attribute's name to the name given in the corresponding cell in the second column.
        RustFfiHtmlNamespace::Svg => match attribute_name {
            "attributename" => "attributeName",
            "attributetype" => "attributeType",
            "basefrequency" => "baseFrequency",
            "baseprofile" => "baseProfile",
            "calcmode" => "calcMode",
            "clippathunits" => "clipPathUnits",
            "diffuseconstant" => "diffuseConstant",
            "edgemode" => "edgeMode",
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
        },
        RustFfiHtmlNamespace::Html => attribute_name,
        RustFfiHtmlNamespace::Other => attribute_name,
    };

    // https://html.spec.whatwg.org/multipage/parsing.html#adjust-foreign-attributes
    // When the steps below require the user agent to adjust foreign attributes for a token, then, if any of the
    // attributes on the token match the strings given in the first column of the following table, let the attribute be
    // a namespaced attribute, with the prefix being the string given in the corresponding cell in the second column,
    // the local name being the string given in the corresponding cell in the third column, and the namespace being the
    // namespace given in the corresponding cell in the fourth column.
    let (local_name, prefix, namespace_) = match adjusted_name {
        "xlink:actuate" => ("actuate", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xlink:arcrole" => ("arcrole", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xlink:href" => ("href", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xlink:role" => ("role", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xlink:show" => ("show", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xlink:title" => ("title", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xlink:type" => ("type", Some("xlink"), RustFfiHtmlAttributeNamespace::XLink),
        "xml:lang" => ("lang", Some("xml"), RustFfiHtmlAttributeNamespace::Xml),
        "xml:space" => ("space", Some("xml"), RustFfiHtmlAttributeNamespace::Xml),
        "xmlns" => ("xmlns", None, RustFfiHtmlAttributeNamespace::Xmlns),
        "xmlns:xlink" => ("xlink", Some("xmlns"), RustFfiHtmlAttributeNamespace::Xmlns),
        _ => (adjusted_name, None, RustFfiHtmlAttributeNamespace::None),
    };

    AdjustedAttributeName {
        local_name: local_name.to_string(),
        prefix,
        namespace_,
    }
}

// https://html.spec.whatwg.org/multipage/parsing.html#mathml-text-integration-point
fn is_mathml_text_integration_point(node: &StackNode) -> bool {
    if node.namespace_ != RustFfiHtmlNamespace::MathMl {
        return false;
    }

    // A node is a MathML text integration point if it is one of the following elements:
    // - A MathML mi element
    // - A MathML mo element
    // - A MathML mn element
    // - A MathML ms element
    // - A MathML mtext element
    matches!(node.local_name.as_str(), "mi" | "mo" | "mn" | "ms" | "mtext")
}

// https://html.spec.whatwg.org/multipage/parsing.html#html-integration-point
fn is_html_integration_point(node: &StackNode) -> bool {
    // A node is an HTML integration point if it is one of the following elements:
    // - A MathML annotation-xml element whose start tag token had an attribute with the name "encoding" whose value was an ASCII case-insensitive match for the string "text/html"
    // - A MathML annotation-xml element whose start tag token had an attribute with the name "encoding" whose value was an ASCII case-insensitive match for the string "application/xhtml+xml"
    if node.namespace_ == RustFfiHtmlNamespace::MathMl
        && node.local_name == "annotation-xml"
        && node
            .attributes
            .iter()
            .find(|attribute| {
                attribute.namespace_ == RustFfiHtmlAttributeNamespace::None
                    && attribute.prefix.is_none()
                    && attribute.local_name == "encoding"
            })
            .is_some_and(|attribute| {
                attribute.value.eq_ignore_ascii_case("text/html")
                    || attribute.value.eq_ignore_ascii_case("application/xhtml+xml")
            })
    {
        return true;
    }

    // - An SVG foreignObject element
    // - An SVG desc element
    // - An SVG title element
    if node.namespace_ == RustFfiHtmlNamespace::Svg {
        return matches!(node.local_name.as_str(), "foreignObject" | "desc" | "title");
    }

    false
}

// https://html.spec.whatwg.org/multipage/parsing.html#has-an-element-in-the-specific-scope
fn is_scope_boundary(node: &StackNode) -> bool {
    if node.namespace_ == RustFfiHtmlNamespace::Html {
        return matches!(
            node.local_name.as_str(),
            "applet" | "caption" | "html" | "table" | "td" | "th" | "marquee" | "object" | "select" | "template"
        );
    }

    is_html_integration_point(node)
        || is_mathml_text_integration_point(node)
        || (node.namespace_ == RustFfiHtmlNamespace::MathMl && node.local_name == "annotation-xml")
}

fn is_foreign_content_breakout_token(token: &Token) -> bool {
    // -> A start tag whose tag name is one of: "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed", "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr", "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt", "u", "ul", "var"
    if token.is_start_tag_one_of(&[
        "b",
        "big",
        "blockquote",
        "body",
        "br",
        "center",
        "code",
        "dd",
        "div",
        "dl",
        "dt",
        "em",
        "embed",
        "h1",
        "h2",
        "h3",
        "h4",
        "h5",
        "h6",
        "head",
        "hr",
        "i",
        "img",
        "li",
        "listing",
        "menu",
        "meta",
        "nobr",
        "ol",
        "p",
        "pre",
        "ruby",
        "s",
        "small",
        "span",
        "strong",
        "strike",
        "sub",
        "sup",
        "table",
        "tt",
        "u",
        "ul",
        "var",
    ]) {
        return true;
    }

    // -> A start tag whose tag name is "font", if the token has any attributes named "color", "face", or "size"
    if token.is_start_tag_named("font")
        && (token.has_attribute("color") || token.has_attribute("face") || token.has_attribute("size"))
    {
        return true;
    }

    // -> An end tag whose tag name is "br", "p"
    token.is_end_tag_one_of(&["br", "p"])
}

fn is_formatting_element(tag_name: &str) -> bool {
    matches!(
        tag_name,
        "a" | "b" | "big" | "code" | "em" | "font" | "i" | "nobr" | "s" | "small" | "strike" | "strong" | "tt" | "u"
    )
}

fn is_implied_end_tag(tag_name: &str) -> bool {
    matches!(
        tag_name,
        "dd" | "dt" | "li" | "optgroup" | "option" | "p" | "rb" | "rp" | "rt" | "rtc"
    )
}

fn is_thoroughly_implied_end_tag(tag_name: &str) -> bool {
    matches!(
        tag_name,
        "caption"
            | "colgroup"
            | "dd"
            | "dt"
            | "li"
            | "optgroup"
            | "option"
            | "p"
            | "rb"
            | "rp"
            | "rt"
            | "rtc"
            | "tbody"
            | "td"
            | "tfoot"
            | "th"
            | "thead"
            | "tr"
    )
}

fn starts_with_ignore_ascii_case(string: &str, prefix: &str) -> bool {
    string
        .get(..prefix.len())
        .is_some_and(|start| start.eq_ignore_ascii_case(prefix))
}

// https://html.spec.whatwg.org/multipage/parsing.html#special
fn is_special_tag(node: &StackNode) -> bool {
    match node.namespace_ {
        RustFfiHtmlNamespace::Html => matches!(
            node.local_name.as_str(),
            "address"
                | "applet"
                | "area"
                | "article"
                | "aside"
                | "base"
                | "basefont"
                | "bgsound"
                | "blockquote"
                | "body"
                | "br"
                | "button"
                | "caption"
                | "center"
                | "col"
                | "colgroup"
                | "dd"
                | "details"
                | "dir"
                | "div"
                | "dl"
                | "dt"
                | "embed"
                | "fieldset"
                | "figcaption"
                | "figure"
                | "footer"
                | "form"
                | "frame"
                | "frameset"
                | "h1"
                | "h2"
                | "h3"
                | "h4"
                | "h5"
                | "h6"
                | "head"
                | "header"
                | "hgroup"
                | "hr"
                | "html"
                | "iframe"
                | "img"
                | "input"
                | "keygen"
                | "li"
                | "link"
                | "listing"
                | "main"
                | "marquee"
                | "menu"
                | "meta"
                | "nav"
                | "noembed"
                | "noframes"
                | "noscript"
                | "object"
                | "ol"
                | "p"
                | "param"
                | "plaintext"
                | "pre"
                | "script"
                | "search"
                | "section"
                | "select"
                | "source"
                | "style"
                | "summary"
                | "table"
                | "tbody"
                | "td"
                | "template"
                | "textarea"
                | "tfoot"
                | "th"
                | "thead"
                | "title"
                | "tr"
                | "track"
                | "ul"
                | "wbr"
                | "xmp"
        ),
        RustFfiHtmlNamespace::Svg => matches!(node.local_name.as_str(), "desc" | "foreignObject" | "title"),
        RustFfiHtmlNamespace::MathMl => matches!(
            node.local_name.as_str(),
            "mi" | "mo" | "mn" | "ms" | "mtext" | "annotation-xml"
        ),
        RustFfiHtmlNamespace::Other => false,
    }
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
            | "keygen"
            | "link"
            | "meta"
            | "param"
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
    fn synthetic_character(code_point: u32) -> Self {
        Self {
            token_type: TokenType::Character,
            code_point,
            ..Default::default()
        }
    }

    fn synthetic_end_tag(tag_name: &str) -> Self {
        Self {
            token_type: TokenType::EndTag,
            payload: TokenPayload::Tag {
                tag_name: tag_name.to_string(),
                tag_name_id: 0,
                self_closing: false,
                had_duplicate_attribute: false,
                attributes: Vec::new(),
            },
            ..Default::default()
        }
    }

    fn synthetic_start_tag(tag_name: &str) -> Self {
        Self {
            token_type: TokenType::StartTag,
            payload: TokenPayload::Tag {
                tag_name: tag_name.to_string(),
                tag_name_id: 0,
                self_closing: false,
                had_duplicate_attribute: false,
                attributes: Vec::new(),
            },
            ..Default::default()
        }
    }
}

/// Create a new Rust HTML parser.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_parser_create() -> *mut RustFfiHtmlParserHandle {
    Box::into_raw(Box::new(RustFfiHtmlParserHandle {
        run_count: 0,
        state: ParserState::new(),
    }))
}

/// Initialize the Rust HTML parser for the HTML fragment parsing algorithm.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
/// `context_local_name_ptr` must point to `context_local_name_len` valid UTF-8 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_begin_fragment(
    handle: *mut RustFfiHtmlParserHandle,
    root: usize,
    context_element: usize,
    context_namespace: RustFfiHtmlNamespace,
    context_namespace_uri_ptr: *const u8,
    context_namespace_uri_len: usize,
    context_local_name_ptr: *const u8,
    context_local_name_len: usize,
    context_attributes: *const RustFfiHtmlParserAttribute,
    context_attribute_count: usize,
    document_quirks_mode: RustFfiHtmlQuirksMode,
    form_element: usize,
) {
    if handle.is_null() || root == 0 {
        return;
    }

    let context_local_name = if context_local_name_ptr.is_null() {
        String::new()
    } else {
        let bytes = unsafe { std::slice::from_raw_parts(context_local_name_ptr, context_local_name_len) };
        String::from_utf8_lossy(bytes).to_string()
    };
    let context_namespace_uri =
        unsafe { namespace_uri_from_ffi(context_namespace, context_namespace_uri_ptr, context_namespace_uri_len) };
    let context_attributes = unsafe { owned_attributes_from_ffi(context_attributes, context_attribute_count) };

    let handle = unsafe { &mut *handle };
    handle.state.begin_fragment(FragmentParsingContext {
        root,
        context_element: StackNode {
            handle: context_element,
            local_name: context_local_name,
            namespace_: context_namespace,
            namespace_uri: context_namespace_uri,
            attributes: context_attributes,
            template_content: None,
        },
        document_quirks_mode,
        form_element,
    });
}

/// Run the Rust HTML parser.
///
/// Returns `ExecuteScript` when tree construction reaches a parser-blocking script
/// and the C++ host needs to run the script before tokenization can continue.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_run_document(
    handle: *mut RustFfiHtmlParserHandle,
    tokenizer: *mut RustFfiTokenizerHandle,
    host: *mut c_void,
    scripting_enabled: bool,
    stop_at_insertion_point: bool,
) -> RustFfiHtmlParserRunResult {
    if handle.is_null() || tokenizer.is_null() || host.is_null() {
        return RustFfiHtmlParserRunResult::Unsupported;
    }
    // Do not materialize long-lived `&mut` references to the parser state or tokenizer here. C++ host callbacks can
    // synchronously execute script, and script can re-enter this function through document.write() or document.close().
    // TreeBuilder uses raw pointers internally so those nested parser runs do not alias an outer Rust `&mut` borrow.
    unsafe {
        (*handle).run_count = (*handle).run_count.wrapping_add(1);
        (*handle).state.scripting_enabled = scripting_enabled;
        (*handle).state.parser_pause_requested = false;
    }
    let tokenizer = NonNull::new(unsafe { addr_of_mut!((*tokenizer).tokenizer) }).unwrap();
    let state = NonNull::new(unsafe { addr_of_mut!((*handle).state) }).unwrap();
    let mut tree_builder = TreeBuilder::new(tokenizer, host, state);
    tree_builder.run(stop_at_insertion_point);
    if unsafe { (*handle).state.pending_script.is_some() } {
        return RustFfiHtmlParserRunResult::ExecuteScript;
    }
    if unsafe { (*handle).state.pending_svg_script.is_some() } {
        return RustFfiHtmlParserRunResult::ExecuteSvgScript;
    }
    RustFfiHtmlParserRunResult::Ok
}

/// Pop all open elements at the end of parsing.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_pop_all_open_elements(handle: *mut RustFfiHtmlParserHandle) {
    if handle.is_null() {
        return;
    }
    while let Some(node) = unsafe { (*handle).state.stack_of_open_elements.pop() } {
        unsafe { ladybird_html_parser_handle_element_popped(node.handle) };
    }
}

/// Visit all DOM nodes that are kept alive by the Rust HTML parser state.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
/// `visitor` must be a valid `GC::Cell::Visitor`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_visit_edges(handle: *const RustFfiHtmlParserHandle, visitor: *mut c_void) {
    if handle.is_null() || visitor.is_null() {
        return;
    }
    let handle = unsafe { &*handle };
    handle.state.visit_edges(visitor);
}

/// Take the script element that caused the last Rust HTML parser run to stop.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_take_pending_script(handle: *mut RustFfiHtmlParserHandle) -> usize {
    if handle.is_null() {
        return 0;
    }
    let handle = unsafe { &mut *handle };
    handle.state.pending_script.take().unwrap_or(0)
}

/// Take the SVG script element that caused the last Rust HTML parser run to stop.
///
/// # Safety
/// `handle` must be a valid pointer from `rust_html_parser_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_parser_take_pending_svg_script(handle: *mut RustFfiHtmlParserHandle) -> usize {
    if handle.is_null() {
        return 0;
    }
    let handle = unsafe { &mut *handle };
    handle.state.pending_svg_script.take().unwrap_or(0)
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
