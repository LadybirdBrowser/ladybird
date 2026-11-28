/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Function.h>
#include <AK/GenericShorthands.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/Node.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/FragmentSerializationMode.h>
#include <LibWeb/DOM/RustFFI.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/Export.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/TraversalDecision.h>
#include <LibWeb/TreeNode.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/DOM/NodeType.h>

namespace Web::DOM {

class Document;

}

namespace Web::CSS {

class StyleScope;
enum class UserSelect : u8;

}

namespace Web::Layout::RustFFI {

struct FfiBoxMarks;

}

namespace Web::Bindings {

struct GetRootNodeOptions;
struct SetHTMLOptions;

}

namespace Web::DOM {

enum class NameOrDescription {
    Name,
    Description
};

enum class IsDescendant {
    No,
    Yes,
};

enum class ShouldComputeRole {
    No,
    Yes,
};

enum class RootNodeComposed {
    No,
    Yes,
};

#define ENUMERATE_SET_NEEDS_LAYOUT_REASONS(X)         \
    X(CharacterDataReplaceData)                       \
    X(DefaultPreferredSizeAttributeChange)            \
    X(EditableStateChange)                            \
    X(FinalizeACrossDocumentNavigation)               \
    X(GeneratedContentImageFinishedLoading)           \
    X(HTMLCanvasElementWidthOrHeightChange)           \
    X(HTMLImageElementReactToChangesInTheEnvironment) \
    X(HTMLImageElementUpdateTheImageData)             \
    X(EmbeddedDocumentResized)                        \
    X(HTMLVideoElementNaturalDimensionsChanged)       \
    X(HTMLVideoElementSetVideoTrack)                  \
    X(ImplicitAnchorChange)                           \
    X(KeyframeEffect)                                 \
    X(LayoutTreeUpdate)                               \
    X(MathMLAttributeChange)                          \
    X(FrameScrollingAttributeChange)                  \
    X(NavigableSetViewportSize)                       \
    X(SVGImageElementFetchTheDocument)                \
    X(SVGResourceElementAttributeChange)              \
    X(SVGViewBoxChange)                               \
    X(StyleChange)

enum class SetNeedsLayoutReason {
#define ENUMERATE_SET_NEEDS_LAYOUT_REASON(e) e,
    ENUMERATE_SET_NEEDS_LAYOUT_REASONS(ENUMERATE_SET_NEEDS_LAYOUT_REASON)
#undef ENUMERATE_SET_NEEDS_LAYOUT_REASON
};

[[nodiscard]] Utf16View to_string(SetNeedsLayoutReason);

#define ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASONS(X) \
    X(CharacterDataReplaceData)                           \
    X(ElementSetShadowRoot)                               \
    X(DetailsElementOpenedOrClosed)                       \
    X(HTMLImageElementUpdateTheImageData)                 \
    X(HTMLInputElementSrcAttribute)                       \
    X(HTMLInputElementTypeAttribute)                      \
    X(HTMLEmbedElementRepresentationChanged)              \
    X(HTMLObjectElementUpdateLayoutAndChildObjects)       \
    X(KeyframeEffect)                                     \
    X(LanguageChangeUnderCasingTextTransform)             \
    X(ListItemCounters)                                   \
    X(PseudoElementChange)                                \
    X(NodeInsertBefore)                                   \
    X(NodeInsertBeforeWithDisplayContents)                \
    X(NodeRemove)                                         \
    X(TableCellSpanAttribute)                             \
    X(None)                                               \
    X(SlotAssignmentChange)                               \
    X(StyleChange)                                        \
    X(StyleRebuild)                                       \
    X(SVGResourceContentChange)                           \
    X(SVGResourceElementRemoved)                          \
    X(SVGResourceReferenceChange)                         \
    X(SVGUseElementShadowTreeRebuild)                     \
    X(TopLayerMembershipChange)                           \
    X(WhitespaceTextBoxChange)                            \
    X(WritingSuggestionChange)

enum class SetNeedsLayoutTreeUpdateReason {
#define ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASON(e) e,
    ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASONS(ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASON)
#undef ENUMERATE_SET_NEEDS_LAYOUT_TREE_UPDATE_REASON
};

[[nodiscard]] Utf16View to_string(SetNeedsLayoutTreeUpdateReason);

class WEB_API Node : public EventTarget
    , public TreeNode<Node> {
    WEB_WRAPPABLE(Node, EventTarget);

public:
    ParentNode* parent_or_shadow_host();
    ParentNode const* parent_or_shadow_host() const { return const_cast<Node*>(this)->parent_or_shadow_host(); }
    Node const* parent_or_shadow_host_node() const;
    Element* parent_or_shadow_host_element();
    Element const* parent_or_shadow_host_element() const { return const_cast<Node*>(this)->parent_or_shadow_host_element(); }
    ParentNode* flat_tree_parent();
    ParentNode const* flat_tree_parent() const { return const_cast<Node*>(this)->flat_tree_parent(); }
    Element* flat_tree_parent_element();
    Element const* flat_tree_parent_element() const { return const_cast<Node*>(this)->flat_tree_parent_element(); }

    virtual ~Node();

    NodeType type() const { return m_core.node_type; }
    bool is_element() const { return type() == NodeType::ELEMENT_NODE; }
    bool is_text() const { return type() == NodeType::TEXT_NODE || type() == NodeType::CDATA_SECTION_NODE; }
    bool is_exclusive_text() const { return type() == NodeType::TEXT_NODE; }
    bool is_document() const { return type() == NodeType::DOCUMENT_NODE; }
    bool is_document_type() const { return type() == NodeType::DOCUMENT_TYPE_NODE; }
    bool is_comment() const { return type() == NodeType::COMMENT_NODE; }
    bool is_character_data() const { return first_is_one_of(type(), NodeType::TEXT_NODE, NodeType::COMMENT_NODE, NodeType::CDATA_SECTION_NODE, NodeType::PROCESSING_INSTRUCTION_NODE); }
    bool is_document_fragment() const { return type() == NodeType::DOCUMENT_FRAGMENT_NODE; }
    bool is_slottable() const { return is_element() || is_text() || is_cdata_section(); }
    bool is_attribute() const { return type() == NodeType::ATTRIBUTE_NODE; }
    bool is_cdata_section() const { return type() == NodeType::CDATA_SECTION_NODE; }
    bool is_shadow_root() const { return fast_is<ShadowRoot>(); }

    bool is_svg_element() const { return fast_is<SVG::SVGElement>(); }
    bool is_svg_graphics_element() const { return fast_is<SVG::SVGGraphicsElement>(); }
    bool is_svg_mask_element() const { return fast_is<SVG::SVGMaskElement>(); }
    bool is_svg_script_element() const { return fast_is<SVG::SVGScriptElement>(); }
    bool is_svg_style_element() const { return fast_is<SVG::SVGStyleElement>(); }
    bool is_svg_svg_element() const { return fast_is<SVG::SVGSVGElement>(); }
    bool is_svg_a_element() const { return fast_is<SVG::SVGAElement>(); }
    bool is_svg_foreign_object_element() const { return fast_is<SVG::SVGForeignObjectElement>(); }
    bool is_svg_gradient_element() const { return fast_is<SVG::SVGGradientElement>(); }
    bool is_svg_pattern_element() const { return fast_is<SVG::SVGPatternElement>(); }
    bool is_svg_clip_path_element() const { return fast_is<SVG::SVGClipPathElement>(); }
    bool is_svg_geometry_element() const { return fast_is<SVG::SVGGeometryElement>(); }
    bool is_svg_filter_element() const { return fast_is<SVG::SVGFilterElement>(); }
    bool is_mathml_anchor_element() const { return fast_is<MathML::MathMLAnchorElement>(); }

    bool in_a_document_tree() const;

    // NOTE: This is intended for the JS bindings.
    u16 node_type() const { return (u16)m_core.node_type; }

    bool is_editable() const;
    bool is_editing_host() const;
    bool is_editable_or_editing_host() const { return is_editable() || is_editing_host(); }
    // NB: A false answer settles is_editable_or_editing_host() without asking what kind of node this is.
    bool may_be_editable_or_editing_host() const;
    GC::Ptr<Node> editing_host();

    bool has_spelling_markers() const { return m_core.cpp_flags.has_spelling_markers; }
    void set_has_spelling_markers(Badge<Editing::SpellingMarkers>, bool value) { m_core.cpp_flags.has_spelling_markers = value; }
    CSS::UserSelect user_select_used_value() const;

    bool in_editable_subtree() const { return m_core.flags & RustFFI::NODE_IN_EDITABLE_SUBTREE; }
    bool recompute_editable_subtree_flag();
    void recompute_editable_subtree_flags_and_repaint();
    // Brings the editing-host and empty-text stamps of the boxes in the subtree to the nodes' editability.

    bool is_html_element() const { return fast_is<HTML::HTMLElement>(); }
    bool is_html_html_element() const { return fast_is<HTML::HTMLHtmlElement>(); }
    bool is_html_anchor_element() const { return fast_is<HTML::HTMLAnchorElement>(); }
    bool is_html_area_element() const { return fast_is<HTML::HTMLAreaElement>(); }
    bool is_html_body_element() const { return fast_is<HTML::HTMLBodyElement>(); }
    bool is_html_input_element() const { return fast_is<HTML::HTMLInputElement>(); }
    bool is_html_link_element() const { return fast_is<HTML::HTMLLinkElement>(); }
    bool is_html_media_element() const { return fast_is<HTML::HTMLMediaElement>(); }
    bool is_html_script_element() const { return fast_is<HTML::HTMLScriptElement>(); }
    bool is_html_style_element() const { return fast_is<HTML::HTMLStyleElement>(); }
    bool is_html_template_element() const { return fast_is<HTML::HTMLTemplateElement>(); }
    bool is_html_details_element() const { return fast_is<HTML::HTMLDetailsElement>(); }
    bool is_html_slot_element() const { return fast_is<HTML::HTMLSlotElement>(); }
    bool is_html_iframe_element() const { return fast_is<HTML::HTMLIFrameElement>(); }
    bool is_html_textarea_element() const { return fast_is<HTML::HTMLTextAreaElement>(); }
    bool is_html_frameset_element() const { return fast_is<HTML::HTMLFrameSetElement>(); }
    bool is_html_li_element() const { return fast_is<HTML::HTMLLIElement>(); }
    bool is_html_menu_element() const { return fast_is<HTML::HTMLMenuElement>(); }
    bool is_html_olist_element() const { return fast_is<HTML::HTMLOListElement>(); }
    bool is_html_ulist_element() const { return fast_is<HTML::HTMLUListElement>(); }
    ALWAYS_INLINE bool is_html_ol_ul_menu_element() const
    {
        return is_html_olist_element() || is_html_ulist_element() || is_html_menu_element();
    }
    bool is_navigable_container() const { return fast_is<HTML::NavigableContainer>(); }
    // NB: HTML::LazyLoadingElement asserts that these are its only classes.
    bool is_lazy_loading() const { return fast_is<HTML::HTMLImageElement>() || fast_is<HTML::HTMLIFrameElement>(); }

    WebIDL::ExceptionOr<GC::Ref<Node>> pre_insert(GC::Ref<Node>, GC::Ptr<Node>);
    WebIDL::ExceptionOr<GC::Ref<Node>> pre_remove(GC::Ref<Node>);

    WebIDL::ExceptionOr<GC::Ref<Node>> append_child(GC::Ref<Node>);
    WebIDL::ExceptionOr<GC::Ref<Node>> remove_child(GC::Ref<Node>);

    void insert_before(GC::Ref<Node> node, GC::Ptr<Node> child, bool suppress_observers = false);
    void parser_insert_before(GC::Ref<Node> node, GC::Ptr<Node> child);
    void parser_append_child(GC::Ref<Node> node) { parser_insert_before(node, nullptr); }
    // Links a node the parser has just created into a new subtree that is not yet in any tree. The caller must
    // insert the subtree's root, which runs the insertion steps for every node of the subtree.
    void parser_append_child_without_insertion_steps(GC::Ref<Node> node)
    {
        VERIFY(!is_connected() && &node->document() == &document());
        append_child_impl(node);
    }
    void remove(bool suppress_observers = false);
    void remove_all_children(bool suppress_observers = false);

    enum DocumentPosition : u16 {
        DOCUMENT_POSITION_EQUAL = 0,
        DOCUMENT_POSITION_DISCONNECTED = 1,
        DOCUMENT_POSITION_PRECEDING = 2,
        DOCUMENT_POSITION_FOLLOWING = 4,
        DOCUMENT_POSITION_CONTAINS = 8,
        DOCUMENT_POSITION_CONTAINED_BY = 16,
        DOCUMENT_POSITION_IMPLEMENTATION_SPECIFIC = 32,
    };

    u16 compare_document_position(GC::Ptr<Node> other);

    WebIDL::ExceptionOr<GC::Ref<Node>> replace_child(GC::Ref<Node> node, GC::Ref<Node> child);

    WebIDL::ExceptionOr<GC::Ref<Node>> clone_node(GC::Ptr<Document> document = nullptr, bool subtree = false, GC::Ptr<Node> parent = nullptr, GC::Ptr<HTML::CustomElementRegistry> fallback_registry = nullptr) const;
    WebIDL::ExceptionOr<GC::Ref<Node>> clone_single_node(Document&, GC::Ptr<HTML::CustomElementRegistry> fallback_registry) const;
    WebIDL::ExceptionOr<GC::Ref<Node>> clone_node(bool subtree);

    WebIDL::ExceptionOr<void> move_node(Node& new_parent, Node* child);

    // NOTE: This is intended for the JS bindings.
    bool has_child_nodes() const { return has_children(); }
    GC::Ref<NodeList> child_nodes();
    GC::RootVector<GC::Ref<Node>> children_as_vector() const;

    virtual Utf16FlyString node_name() const = 0;

    Utf16String base_uri() const;

    virtual Optional<Utf16String> alternative_text() const;

    Utf16String descendant_text_content() const;
    Optional<Utf16String> text_content() const;
    WebIDL::ExceptionOr<void> set_text_content(Optional<Utf16String> const&);

    WebIDL::ExceptionOr<void> normalize();

    Optional<Utf16String> node_value() const;
    WebIDL::ExceptionOr<void> set_node_value(Optional<Utf16String> const&);

    GC::Ptr<HTML::LocalNavigable> navigable() const;

    Document& document() { return *m_core.document; }
    Document const& document() const { return *m_core.document; }

    // AD-HOC: Version counters of the tree this node is in, for caches that depend on tree structure or contents.
    //         The DOM tree version moves whenever a node is inserted into or removed from the tree, or an element
    //         attribute in it changes; the character data version whenever CharacterData in it is modified.
    //         A connected node's tree is its document; a disconnected node's is the tree under its root. Counting
    //         per tree keeps a document's churn from invalidating caches keyed on disconnected trees, and vice versa.
    //         Every bump takes a stamp no other tree has had, so a version remembered from one tree is never
    //         mistaken for the version of another.
    u64 dom_tree_version() const;
    u64 character_data_version() const;
    void bump_dom_tree_version();
    void bump_character_data_version();

    GC::Ptr<Document> owner_document() const;

    HTML::HTMLHyperlinkElementUtils const* enclosing_link_element() const;
    HTML::HTMLElement const* enclosing_html_element() const;

    Utf16String child_text_content() const;

    Node& shadow_including_root();
    Node const& shadow_including_root() const
    {
        return const_cast<Node*>(this)->shadow_including_root();
    }

    Node& root() { return *m_core.root; }
    Node const& root() const { return *m_core.root; }

    bool is_closed_shadow_hidden_from(Node const&) const;

    bool is_connected() const { return m_core.flags & RustFFI::NODE_IS_CONNECTED; }
    void set_is_connected(bool is_connected) { set_core_flag(RustFFI::NODE_IS_CONNECTED, is_connected); }

    bool is_tracked_by_style_engine() const;

    // The node's identity in its document's style engine, which only an element, a text node, a shadow root or a
    // document takes.
    [[nodiscard]] CSS::StyleNodeID style_node_id() const { return m_core.style_node_id; }

    // Whether this subtree waits to take its place in the style engine's tree, and whether a shadow-including
    // descendant's subtree does. See CSS::take_in_pending_style_arrivals().
    bool style_arrival_pending() const { return m_core.cpp_flags.style_arrival_pending; }
    void set_style_arrival_pending(bool value) { m_core.cpp_flags.style_arrival_pending = value; }
    bool descendant_style_arrival_pending() const { return m_core.cpp_flags.descendant_style_arrival_pending; }
    void set_descendant_style_arrival_pending(bool value) { m_core.cpp_flags.descendant_style_arrival_pending = value; }
    // Whether this element arrived alone in a display:none subtree, its children waiting to arrive until something
    // could observe them. See CSS::take_in_pending_style_arrivals().
    bool child_style_arrivals_wait() const { return m_core.cpp_flags.child_style_arrivals_wait; }
    void set_child_style_arrivals_wait(bool value) { m_core.cpp_flags.child_style_arrivals_wait = value; }

    // Mirrors the slottable's assigned slot; see SlottableMixin::set_assigned_slot().
    bool has_assigned_slot() const { return m_core.cpp_flags.has_assigned_slot; }
    void set_has_assigned_slot(Badge<SlottableMixin>, bool value) { m_core.cpp_flags.has_assigned_slot = value; }

    bool inside_blocking_wheel_event_handler() const { return m_core.cpp_flags.inside_blocking_wheel_event_handler; }
    bool update_inside_blocking_wheel_event_handler_state();
    void update_inside_blocking_wheel_event_handler_state_for_subtree();

    [[nodiscard]] bool is_browsing_context_connected() const;

    Node* parent_node() { return parent(); }
    Node const* parent_node() const { return parent(); }

    static constexpr GC::MemberOffset parent_node_offset() { return GC_FIELD_OFFSET(Node, m_core.parent); }
    static constexpr GC::MemberOffset first_child_offset() { return GC_FIELD_OFFSET(Node, m_core.first_child); }
    static constexpr GC::MemberOffset last_child_offset() { return GC_FIELD_OFFSET(Node, m_core.last_child); }
    static constexpr GC::MemberOffset previous_sibling_offset() { return GC_FIELD_OFFSET(Node, m_core.previous_sibling); }
    static constexpr GC::MemberOffset next_sibling_offset() { return GC_FIELD_OFFSET(Node, m_core.next_sibling); }

    GC::Ptr<Element> parent_element();
    GC::Ptr<Element const> parent_element() const;

    MUST_UPCALL virtual void inserted();
    virtual void post_connection();
    // Whether this node overrides post_connection(). Every override of post_connection() must also override this to
    // return true, since insertions skip the post-connection steps when no inserted node has them.
    virtual bool has_post_connection_steps() const { return false; }
    using IsSubtreeRoot = RustFFI::IsSubtreeRoot;
    MUST_UPCALL virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root);
    MUST_UPCALL virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor);

    struct ChildrenChangedMetadata {
        enum class Type {
            Inserted,
            Removal,
            AllChildrenRemoved,
            Mutation,
        };
        Type type {};
        GC::Ref<Node> node;
        // For a removal, the sibling that was before the removed node, if any.
        GC::Ptr<Node> old_previous_sibling {};
    };
    // FIXME: It would be good if we could always provide this metadata for use in optimizations.
    virtual void children_changed(ChildrenChangedMetadata const&) { }

    virtual void adopted_from(Document&) { }
    virtual WebIDL::ExceptionOr<void> cloned(Node&, bool) const { return {}; }

    Layout::Node const* layout_node(Layout::BegunRead const& read) const;
    Layout::Node* layout_node(Layout::BegunRead const& read);

    Layout::Node const* unsafe_layout_node(Layout::BegunRead const& read) const;
    Layout::Node* unsafe_layout_node(Layout::BegunRead const& read) { return const_cast<Layout::Node*>(static_cast<Node const*>(this)->unsafe_layout_node(read)); }
    // Whether the last layout tree build gave this node a box, and whether layout committed geometry for it. Code
    // that only needs to know whether there is a box should ask these instead of reaching for the box. The layout
    // node arena writes both as it changes the boxes they describe, so asking reads the node, not the arena.
    [[nodiscard]] bool has_layout_box() const { return m_core.cpp_flags.has_layout_box; }
    [[nodiscard]] bool may_have_layout_box() const;
    [[nodiscard]] bool is_rendered() const { return m_core.cpp_flags.has_committed_box; }
    void set_box_presence(Badge<Layout::NodeArena>, bool has_layout_box, bool has_committed_box)
    {
        m_core.cpp_flags.has_layout_box = has_layout_box;
        m_core.cpp_flags.has_committed_box = has_committed_box;
    }
    void set_needs_repaint(InvalidateDisplayList = InvalidateDisplayList::PaintCommandsAndHitTestList);
    // Queues `marks` for the node's box, which the render state finds as it applies them: the marks may be made beside a
    // frame in flight, which holds the boxes.
    void mark_box(Layout::RustFFI::FfiBoxMarks);
    // The facts about this node that a box built for it paints (inertness, editability, and so on) may have changed.
    void publish_dom_paint_facts();
    void set_needs_layout_update(SetNeedsLayoutReason);

    // Does what a removal does to the layout nodes of this subtree that stay until the parent's layout is rebuilt,
    // while the StyleNodeIDs they are found through are still current.
    void detach_remaining_layout_nodes_for_removal();

    virtual bool is_child_allowed(Node const&) const { return true; }

    // The layout tree update marks live in the layout arena, keyed by the node's identity in the style mirror. A node
    // the style mirror has not named holds none.
    [[nodiscard]] bool needs_layout_tree_update() const;
    void set_needs_layout_tree_update(bool, SetNeedsLayoutTreeUpdateReason);
    // Which narrower rebuild a layout tree update mark made for `reason` permits.
    static u8 layout_tree_update_reuse_reason(SetNeedsLayoutTreeUpdateReason);

    [[nodiscard]] bool may_reuse_layout_node_for_child_list_insertion() const { return layout_tree_update_reuse_reasons() & ChildListInsertion; }

    // The number of animations associated with this node's shadow-including inclusive subtree. A
    // synchronous read of layout geometry has to catch up the style of a throttled animation that
    // could change what it sees, and this count answers "there is none in here" for a whole subtree
    // at once, so a read far away from the page's animations never looks at an animation.
    [[nodiscard]] u32 associated_animation_count_in_subtree() const { return m_associated_animation_count_in_subtree; }
    void change_associated_animation_count_in_subtree(i32 delta);

    CSS::StyleScope& style_scope();
    CSS::StyleScope const& style_scope() const { return const_cast<Node*>(this)->style_scope(); }

    void set_document(Badge<Document, NamedNodeMap>, Document&);

    virtual EventTarget* get_parent(Event const&) override;

    // Whether the node is of class T, or of a class that descends from it, which its class id tells. See
    // DOM/NodeClassId.h.
    template<typename T>
    requires(HasNodeClassId<T>)
    bool fast_is() const
    {
        return node_class_id_is_a<T>(node_class_id());
    }
    // NB: A class of nodes that is no class of its own, such as a mixin, specializes this.
    template<typename T>
    requires(!HasNodeClassId<T>)
    bool fast_is() const = delete;

    template<typename T>
    T* fast_as() = delete;
    template<typename T>
    T const* fast_as() const = delete;

    using ChildrenToExclude = RustFFI::ChildrenToExclude;
    WebIDL::ExceptionOr<void> ensure_pre_insertion_validity(GC::Ref<Node> node, GC::Ptr<Node> child, ChildrenToExclude = ChildrenToExclude::Nothing) const;

    bool is_host_including_inclusive_ancestor_of(Node const&) const;

    bool is_scripting_enabled() const;
    bool is_scripting_disabled() const;

    // Used for dumping the DOM Tree
    void serialize_tree_as_json(JsonObjectSerializer<Utf16StringBuilder>&) const;
    IterationDecision serialize_child_as_json(JsonArraySerializer<Utf16StringBuilder>& children_array, Node const& child) const;

    bool is_shadow_including_descendant_of(Node const&) const;
    bool is_shadow_including_inclusive_descendant_of(Node const&) const;
    bool is_shadow_including_ancestor_of(Node const&) const;
    bool is_shadow_including_inclusive_ancestor_of(Node const&) const;

    [[nodiscard]] UniqueNodeID unique_id() const;
    static Node* from_unique_id(UniqueNodeID);

    Optional<String> webdriver_node_id() const;
    void set_webdriver_node_id(String) const;

    WebIDL::ExceptionOr<Utf16String> serialize_fragment(HTML::RequireWellFormed, FragmentSerializationMode = FragmentSerializationMode::Inner) const;

    // https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-parsing-and-serialization
    // NB: These are the NonDocumentTypeChildNode methods, which are exposed on Element and CharacterData objects.
    WebIDL::ExceptionOr<void> before_html(Utf16String const& html, Bindings::SetHTMLOptions const&);
    WebIDL::ExceptionOr<void> after_html(Utf16String const& html, Bindings::SetHTMLOptions const&);
    WebIDL::ExceptionOr<void> replace_with_html(Utf16String const& html, Bindings::SetHTMLOptions const&);

    void replace_all(GC::Ptr<Node>);
    void replace_all(GC::RootVector<GC::Ref<Node>>);
    void string_replace_all(Utf16View);
    void string_replace_all(Utf16String);

    bool is_same_node(GC::Ptr<Node const>) const;
    bool is_equal_node(GC::Ptr<Node const>) const;

    GC::Ref<Node> get_root_node(RootNodeComposed = RootNodeComposed::No);
    GC::Ref<Node> get_root_node(Bindings::GetRootNodeOptions const&);

    bool is_uninteresting_whitespace_node() const;

    Utf16String debug_description() const;

    size_t length() const;

    Vector<GC::Ref<RegisteredObserver>>* registered_observer_list();
    Vector<GC::Ref<RegisteredObserver>> const* registered_observer_list() const;

    void add_registered_observer(RegisteredObserver&);
    // Removes the registered observers for which callback answers true from the node's registered observer list.
    template<typename Callback>
    void remove_registered_observers_if(Callback callback)
    {
        auto* registered_observer_list = this->registered_observer_list();
        if (!registered_observer_list)
            return;
        registered_observer_list->remove_all_matching(callback);
        if (registered_observer_list->is_empty())
            set_core_flag(RustFFI::NODE_HAS_REGISTERED_OBSERVERS, false);
    }
    // Whether the node or one of its ancestors has registered observers, which libweb_rust reads as well.
    bool has_observed_inclusive_ancestor() const;

    void queue_mutation_record(Utf16FlyString const& type, Optional<Utf16FlyString> const& attribute_name, Optional<Utf16FlyString> const& attribute_namespace, Optional<Utf16String> const& old_value, ReadonlySpan<GC::Ref<Node>> added_nodes, ReadonlySpan<GC::Ref<Node>> removed_nodes, Node* previous_sibling, Node* next_sibling);

    // https://dom.spec.whatwg.org/#concept-shadow-including-inclusive-descendant
    template<typename Callback>
    TraversalDecision for_each_shadow_including_inclusive_descendant(Callback);

    // https://dom.spec.whatwg.org/#concept-shadow-including-descendant
    template<typename Callback>
    TraversalDecision for_each_shadow_including_descendant(Callback);

    Slottable as_slottable();

    template<typename U, typename Callback>
    WebIDL::ExceptionOr<void> for_each_child_of_type_fallible(Callback callback)
    {
        for (auto* node = first_child(); node; node = node->next_sibling()) {
            if (auto* maybe_node_of_type = as_if<U>(node)) {
                if (TRY(callback(*maybe_node_of_type)) == IterationDecision::Break)
                    return {};
            }
        }
        return {};
    }
    template<typename U>
    U const* first_flat_tree_ancestor_of_type() const
    {
        return const_cast<Node*>(this)->template first_flat_tree_ancestor_of_type<U>();
    }

    template<typename U>
    U* first_flat_tree_ancestor_of_type();

    template<typename Predicate>
    requires requires(Predicate& predicate, Node const& node) { { predicate(node) } -> ConvertibleTo<bool>; }
    Node const* find_in_shadow_including_ancestry(Predicate&& predicate) const
    {
        for (Node const* it = this; it; it = it->parent_or_shadow_host_node()) {
            if (predicate(*it))
                return it;
        }
        return nullptr;
    }

    ErrorOr<Utf16String> accessible_name(Document const&, ShouldComputeRole = ShouldComputeRole::Yes) const;
    ErrorOr<Utf16String> accessible_description(Document const&) const;

    Optional<Utf16String> locate_a_namespace(Optional<Utf16View> prefix) const;
    Optional<Utf16String> lookup_namespace_uri(Optional<Utf16String> const& prefix) const;
    Optional<Utf16String> lookup_namespace_uri(Optional<Utf16View> prefix) const;
    Optional<Utf16String> lookup_prefix(Optional<Utf16String> const& namespace_) const;
    Optional<Utf16String> lookup_prefix(Optional<Utf16View> namespace_) const;
    bool is_default_namespace(Optional<Utf16String> const& namespace_) const;
    bool is_default_namespace(Optional<Utf16View> namespace_) const;
    Vector<Utf16FlyString> get_in_scope_prefixes() const;

    bool is_inert() const;

    bool has_inclusive_ancestor_with_display_none_ignoring_animations() const;
    GC::Ptr<ShadowRoot> containing_shadow_root();
    GC::Ptr<ShadowRoot const> containing_shadow_root() const
    {
        return const_cast<Node*>(this)->containing_shadow_root();
    }

protected:
    friend class HTMLCollection;

    struct RareData {
        AK_ALLOC_WITH_KMALLOC;

        virtual ~RareData();
        virtual void visit_edges(Cell::Visitor&);
        virtual size_t external_memory_size() const;

        Optional<String> webdriver_node_id;

        // https://dom.spec.whatwg.org/#registered-observer-list
        // "Nodes have a strong reference to registered observers in their registered observer list." https://dom.spec.whatwg.org/#garbage-collection
        OwnPtr<Vector<GC::Ref<RegisteredObserver>>> registered_observer_list;

        GC::Member<NodeList> child_nodes;
        GC::Member<HTMLCollection> children;

        // The collections getElementsByTagName() returned, which it may return again for the same argument while the
        // type of the root's node document stays the same, so that type is kept with each.
        struct CollectionByTagName {
            GC::Weak<HTMLCollection> collection;
            bool for_html_document { false };
        };
        OwnPtr<HashMap<Utf16FlyString, CollectionByTagName>> collections_by_tag_name;
        // The size the map grows to before the entries of collected collections are swept from it.
        size_t collections_by_tag_name_sweep_size { 0 };
    };

    Node(Document&, NodeType);

    void set_document(Document&);

    virtual OwnPtr<RareData> create_rare_data() const;
    RareData& ensure_rare_data() const;
    RareData* rare_data() { return m_rare_data; }
    RareData const* rare_data() const { return m_rare_data; }

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;
    virtual size_t external_memory_size() const override;

    // The tree state of the node, whose layout libweb_rust defines (see Rust/src/dom/node_core.rs), which reads it in
    // place.
    RustFFI::NodeCore m_core {};

    void set_core_flag(u8 flag, bool value) { m_core.flags = static_cast<u8>(value ? (m_core.flags | flag) : (m_core.flags & ~flag)); }
    // Which narrower rebuilds the layout tree update marks collected so far still permit.
    enum LayoutTreeUpdateReuseReason : u8 {
        ChildListInsertion = 1,
        PseudoElementChange = 2,
        // NB: Bits 4 and 8 are the marks themselves, and 32 and 64 notes of the mark.
        ChildListRemoval = 16,
        // The element's box may stay if a fresh build gives it a box of the same kind.
        Restyle = 128,
    };
    [[nodiscard]] u8 layout_tree_update_reuse_reasons() const;

    u32 m_associated_animation_count_in_subtree { 0 };
    // The slot of the node directory that names the node by its unique id, or 0 before anything asked for the id.
    mutable u32 m_node_directory_slot { 0 };

    void build_accessibility_tree(AccessibilityTreeNode& parent);

    ErrorOr<Utf16String> name_or_description(Layout::BegunRead const& read, NameOrDescription, Document const&, HashTable<UniqueNodeID>&, IsDescendant = IsDescendant::No, ShouldComputeRole = ShouldComputeRole::Yes) const;
    // The first ID token of an aria-labelledby or aria-describedby value that names an element in the document.
    static Optional<Utf16View> first_valid_id(Utf16View, Document const&);

private:
    friend class TreeNode<Node>;
    friend class RustDOMHooks;
    RustFFI::NodeCore& tree_node_links() { return m_core; }
    RustFFI::NodeCore const& tree_node_links() const { return m_core; }

    bool schedule_list_item_renumber_for_removal();
    void report_removal_to_style_engine(Node& parent);
    void update_layout_tree_for_removal(Node& parent);
    GC::RootVector<GC::Ref<HTML::HTMLSlotElement>> descendant_slots_before_removal(Node& parent_root);
    bool settle_descendant_slots_before_removal(Node& parent_root);
    void assign_slottables_after_removal(Node& parent, Node& parent_root, ReadonlySpan<GC::Ref<HTML::HTMLSlotElement>> descendant_slots);
    void add_transient_registered_observers(Node& parent);
    void queue_tree_mutation_record_for_removal(Node& parent, GC::Ptr<Node> old_previous_sibling, GC::Ptr<Node> old_next_sibling);

    void queue_tree_mutation_record(ReadonlySpan<GC::Ref<Node>> added_nodes, ReadonlySpan<GC::Ref<Node>> removed_nodes, Node* previous_sibling, Node* next_sibling);

    void insert_before_impl(GC::Ref<Node>, GC::Ptr<Node> child);
    void insert_node_into_children(GC::Ref<Node>, GC::Ptr<Node> child);
    void append_child_impl(GC::Ref<Node>);
    void prepare_to_unlink_child(Node&);
    void account_for_linked_child(Node&);

    void begin_move(MoveScope&, Node& old_parent, Node& old_parent_root, Node& new_parent);
    void after_move_unlink(MoveScope&, Node& old_parent, Node& old_parent_root);
    void after_move_link(MoveScope&, Node& old_parent, Node& old_parent_root, Node& new_parent);
    void end_move(MoveScope&, Node& old_parent, Node& new_parent, GC::Ptr<Node> old_previous_sibling, GC::Ptr<Node> old_next_sibling, GC::Ptr<Node> new_previous_sibling, GC::Ptr<Node> child);

    mutable OwnPtr<RareData> m_rare_data;
};

// NB: The Rust definition of the core asserts the same layout.
static_assert(sizeof(RustFFI::NodeCore) == 40 && alignof(RustFFI::NodeCore) == 4);
static_assert(offsetof(RustFFI::NodeCore, parent) == 0);
static_assert(offsetof(RustFFI::NodeCore, first_child) == 4);
static_assert(offsetof(RustFFI::NodeCore, last_child) == 8);
static_assert(offsetof(RustFFI::NodeCore, next_sibling) == 12);
static_assert(offsetof(RustFFI::NodeCore, previous_sibling) == 16);
static_assert(offsetof(RustFFI::NodeCore, document) == 20);
static_assert(offsetof(RustFFI::NodeCore, root) == 24);
static_assert(offsetof(RustFFI::NodeCore, style_node_id) == 28);
static_assert(offsetof(RustFFI::NodeCore, node_type) == 32);
static_assert(offsetof(RustFFI::NodeCore, flags) == 34);
static_assert(offsetof(RustFFI::NodeCore, cpp_flags) == 35);
static_assert(offsetof(RustFFI::NodeCore, child_index_generation) == 36);
// NB: libweb_rust finds the core of a node at this offset into the node's cell.
static_assert(Node::parent_node_offset().value == RustFFI::NODE_CORE_OFFSET + offsetof(RustFFI::NodeCore, parent));

}

namespace Web::DOM {

// Whether DOM/NodeClasses.json lists T, and every class T derives from, under the class it derives from.
template<typename T>
constexpr bool node_class_is_listed_under_its_base()
{
    if constexpr (!HasNodeClassId<T>)
        return false;
    else if constexpr (IsSame<T, Node>)
        return NodeClassTraits<T>::parent == NodeClassId::NotANode;
    else
        return NodeClassTraits<T>::parent == NodeClassTraits<typename T::Base>::id && node_class_is_listed_under_its_base<typename T::Base>();
}

}

namespace GC {

// Gives a node, as it is constructed, the id of its class (see DOM/NodeClassId.h), and tells libweb_rust whether its
// class has tree steps of its own, which the class's overrides decide (see Rust/src/dom/steps.rs).
template<typename T>
requires(IsBaseOf<Web::DOM::Node, T>)
struct CellConstructionHook<T> {
    // Whether T has insertion steps, removing steps, moving steps or post-connection steps beyond those of DOM::Node,
    // DOM::Element and HTML::HTMLElement, which libweb_rust knows to do nothing for some nodes, or is form-associated,
    // whose steps HTML::HTMLElement runs, or is a body or frameset element, whose moving steps DOM::Node runs.
    static constexpr bool has_own_tree_steps()
    {
        using namespace Web;
        using DOM::Node;
        using IsSubtreeRoot = Node::IsSubtreeRoot;
        if (!IsOneOf<decltype(&T::inserted), void (Node::*)(), void (DOM::Element::*)(), void (HTML::HTMLElement::*)()>)
            return true;
        if (!IsOneOf<decltype(&T::removed_from), void (Node::*)(IsSubtreeRoot, Node*, Node&), void (DOM::Element::*)(IsSubtreeRoot, Node*, Node&), void (HTML::HTMLElement::*)(IsSubtreeRoot, Node*, Node&)>)
            return true;
        if (!IsOneOf<decltype(&T::moved_from), void (Node::*)(IsSubtreeRoot, GC::Ptr<Node>), void (DOM::Element::*)(IsSubtreeRoot, GC::Ptr<Node>), void (HTML::HTMLElement::*)(IsSubtreeRoot, GC::Ptr<Node>)>)
            return true;
        if (!IsSame<decltype(&T::is_html_body_element), bool (Node::*)() const> || !IsSame<decltype(&T::is_html_frameset_element), bool (Node::*)() const>)
            return true;
        if (!IsSame<decltype(&T::has_post_connection_steps), bool (Node::*)() const>)
            return true;
        if constexpr (requires { &T::is_form_associated_element; })
            return !IsSame<decltype(&T::is_form_associated_element), bool (HTML::FormAssociatedElement::*)() const>;
        return false;
    }

    static void did_construct(T& node)
    {
        static_assert(Web::DOM::HasNodeClassId<T>, "DOM/NodeClasses.json must list every class of nodes");
        static_assert(Web::DOM::node_class_is_listed_under_its_base<T>(), "DOM/NodeClasses.json must list each class of nodes under the class it derives from");
        static_cast<Web::Bindings::Wrappable&>(node).set_node_class_id(Web::DOM::NodeClassTraits<T>::id);

        if constexpr (IsBaseOf<Web::DOM::Element, T>) {
            if constexpr (has_own_tree_steps())
                node.set_has_own_tree_steps();
        } else {
            // NB: libweb_rust takes the steps of character data and doctypes to be those of every node.
            static_assert(!(IsBaseOf<Web::DOM::CharacterData, T> || IsBaseOf<Web::DOM::DocumentType, T>) || !has_own_tree_steps());
        }
    }
};

}
