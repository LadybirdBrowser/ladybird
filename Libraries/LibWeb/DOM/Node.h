/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Function.h>
#include <AK/GenericShorthands.h>
#include <AK/RefPtr.h>
#include <AK/TypeCasts.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16StringBuilder.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibWeb/Bindings/Node.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/FragmentSerializationMode.h>
#include <LibWeb/DOM/HTMLCollectionCacheRegistration.h>
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

namespace Web::Layout {

enum class LayoutUpdatePropagation : u8;

}

namespace Web::Bindings {

struct GetRootNodeOptions;

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
    X(HTMLObjectElementContentDocumentResized)        \
    X(HTMLVideoElementNaturalDimensionsChanged)       \
    X(HTMLVideoElementSetVideoTrack)                  \
    X(KeyframeEffect)                                 \
    X(LayoutTreeUpdate)                               \
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
    X(ElementSetInnerHTML)                                \
    X(ElementSetShadowRoot)                               \
    X(DetailsElementOpenedOrClosed)                       \
    X(HTMLImageElementUpdateTheImageData)                 \
    X(HTMLInputElementSrcAttribute)                       \
    X(HTMLObjectElementUpdateLayoutAndChildObjects)       \
    X(KeyframeEffect)                                     \
    X(LanguageChangeUnderCasingTextTransform)             \
    X(ListItemCounters)                                   \
    X(PseudoElementChange)                                \
    X(NodeInsertBefore)                                   \
    X(NodeInsertBeforeWithDisplayContents)                \
    X(NodeRemove)                                         \
    X(NodeSetTextContent)                                 \
    X(None)                                               \
    X(PseudoElementBoxEscapedRebuildRoot)                 \
    X(ShadowRootSetInnerHTML)                             \
    X(SlotAssignmentChange)                               \
    X(StyleChange)                                        \
    X(SVGResourceContentChange)                           \
    X(SVGResourceElementRemoved)                          \
    X(TopLayerMembershipChange)

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

    NodeType type() const { return m_type; }
    bool is_element() const { return type() == NodeType::ELEMENT_NODE; }
    bool is_text() const { return type() == NodeType::TEXT_NODE || type() == NodeType::CDATA_SECTION_NODE; }
    bool is_exclusive_text() const { return type() == NodeType::TEXT_NODE; }
    bool is_document() const { return type() == NodeType::DOCUMENT_NODE; }
    bool is_document_type() const { return type() == NodeType::DOCUMENT_TYPE_NODE; }
    bool is_comment() const { return type() == NodeType::COMMENT_NODE; }
    bool is_character_data() const { return first_is_one_of(type(), NodeType::TEXT_NODE, NodeType::COMMENT_NODE, NodeType::CDATA_SECTION_NODE, NodeType::PROCESSING_INSTRUCTION_NODE); }
    bool is_document_fragment() const { return type() == NodeType::DOCUMENT_FRAGMENT_NODE; }
    bool is_parent_node() const { return is_element() || is_document() || is_document_fragment(); }
    bool is_slottable() const { return is_element() || is_text() || is_cdata_section(); }
    bool is_attribute() const { return type() == NodeType::ATTRIBUTE_NODE; }
    bool is_cdata_section() const { return type() == NodeType::CDATA_SECTION_NODE; }
    virtual bool is_shadow_root() const { return false; }

    virtual bool requires_svg_container() const { return false; }
    virtual bool is_svg_container() const { return false; }
    virtual bool is_svg_element() const { return false; }
    virtual bool is_svg_graphics_element() const { return false; }
    virtual bool is_svg_mask_element() const { return false; }
    virtual bool is_svg_script_element() const { return false; }
    virtual bool is_svg_style_element() const { return false; }
    virtual bool is_svg_svg_element() const { return false; }
    virtual bool is_svg_switch_element() const { return false; }
    virtual bool is_svg_symbol_element() const { return false; }
    virtual bool is_svg_use_element() const { return false; }
    virtual bool is_svg_view_element() const { return false; }
    virtual bool is_svg_a_element() const { return false; }
    virtual bool is_svg_g_element() const { return false; }
    virtual bool is_svg_foreign_object_element() const { return false; }
    virtual bool is_svg_gradient_element() const { return false; }
    virtual bool is_svg_pattern_element() const { return false; }
    virtual bool is_svg_clip_path_element() const { return false; }
    virtual bool is_svg_image_element() const { return false; }
    virtual bool is_svg_text_content_element() const { return false; }
    virtual bool is_svg_path_element() const { return false; }
    virtual bool is_svg_rect_element() const { return false; }
    virtual bool is_svg_circle_element() const { return false; }
    virtual bool is_svg_ellipse_element() const { return false; }
    virtual bool is_svg_polyline_element() const { return false; }
    virtual bool is_svg_polygon_element() const { return false; }
    virtual bool is_svg_line_element() const { return false; }
    virtual bool is_svg_text_positioning_element() const { return false; }
    virtual bool is_svg_text_element() const { return false; }
    virtual bool is_svg_text_path_element() const { return false; }

    bool in_a_document_tree() const;

    // NOTE: This is intended for the JS bindings.
    u16 node_type() const { return (u16)m_type; }

    bool is_editable() const;
    bool is_editing_host() const;
    bool is_editable_or_editing_host() const { return is_editable() || is_editing_host(); }
    GC::Ptr<Node> editing_host();
    CSS::UserSelect user_select_used_value() const;

    bool in_editable_subtree() const { return m_in_editable_subtree; }
    bool recompute_editable_subtree_flag();
    void recompute_editable_subtree_flags_and_repaint();
    // Brings the editing-host and empty-text stamps of the boxes in the subtree to the nodes' editability.
    void apply_editability_to_boxes(Badge<InvalidationJournal>, Layout::BegunRead const&);

    virtual bool is_dom_node() const final { return true; }
    virtual bool is_html_element() const { return false; }
    virtual bool is_html_html_element() const { return false; }
    virtual bool is_html_anchor_element() const { return false; }
    virtual bool is_html_area_element() const { return false; }
    virtual bool is_html_base_element() const { return false; }
    virtual bool is_html_body_element() const { return false; }
    virtual bool is_html_head_element() const { return false; }
    virtual bool is_html_heading_element() const { return false; }
    virtual bool is_html_input_element() const { return false; }
    virtual bool is_html_link_element() const { return false; }
    virtual bool is_html_media_element() const { return false; }
    virtual bool is_html_optgroup_element() const { return false; }
    virtual bool is_html_option_element() const { return false; }
    virtual bool is_html_progress_element() const { return false; }
    virtual bool is_html_script_element() const { return false; }
    virtual bool is_html_select_element() const { return false; }
    virtual bool is_html_style_element() const { return false; }
    virtual bool is_html_template_element() const { return false; }
    virtual bool is_html_table_element() const { return false; }
    virtual bool is_html_table_section_element() const { return false; }
    virtual bool is_html_table_row_element() const { return false; }
    virtual bool is_html_table_cell_element() const { return false; }
    virtual bool is_html_table_col_element() const { return false; }
    virtual bool is_html_title_element() const { return false; }
    virtual bool is_html_br_element() const { return false; }
    virtual bool is_html_button_element() const { return false; }
    virtual bool is_html_details_element() const { return false; }
    virtual bool is_html_dialog_element() const { return false; }
    virtual bool is_html_slot_element() const { return false; }
    virtual bool is_html_embed_element() const { return false; }
    virtual bool is_html_object_element() const { return false; }
    virtual bool is_html_canvas_element() const { return false; }
    virtual bool is_html_form_element() const { return false; }
    virtual bool is_html_image_element() const { return false; }
    virtual bool is_html_video_element() const { return false; }
    virtual bool is_html_iframe_element() const { return false; }
    virtual bool is_html_div_element() const { return false; }
    virtual bool is_html_span_element() const { return false; }
    virtual bool is_html_textarea_element() const { return false; }
    virtual bool is_html_frameset_element() const { return false; }
    virtual bool is_html_fieldset_element() const { return false; }
    virtual bool is_html_data_list_element() const { return false; }
    virtual bool is_html_meter_element() const { return false; }
    virtual bool is_html_li_element() const { return false; }
    virtual bool is_html_menu_element() const { return false; }
    virtual bool is_html_olist_element() const { return false; }
    virtual bool is_html_ulist_element() const { return false; }
    ALWAYS_INLINE bool is_html_ol_ul_menu_element() const
    {
        return is_html_olist_element() || is_html_ulist_element() || is_html_menu_element();
    }
    virtual bool is_navigable_container() const { return false; }
    virtual bool is_lazy_loading() const { return false; }

    WebIDL::ExceptionOr<GC::Ref<Node>> pre_insert(GC::Ref<Node>, GC::Ptr<Node>);
    WebIDL::ExceptionOr<GC::Ref<Node>> pre_remove(GC::Ref<Node>);

    WebIDL::ExceptionOr<GC::Ref<Node>> append_child(GC::Ref<Node>);
    WebIDL::ExceptionOr<GC::Ref<Node>> remove_child(GC::Ref<Node>);

    void insert_before(GC::Ref<Node> node, GC::Ptr<Node> child, bool suppress_observers = false);
    void parser_insert_before(GC::Ref<Node> node, GC::Ptr<Node> child);
    void parser_append_child(GC::Ref<Node> node) { parser_insert_before(node, nullptr); }
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

    Document& document() { return *m_document; }
    Document const& document() const { return *m_document; }

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

    Node& root() { return *m_root; }
    Node const& root() const { return *m_root; }

    bool is_closed_shadow_hidden_from(Node const&) const;

    bool is_connected() const { return m_is_connected; }
    void set_is_connected(bool is_connected) { m_is_connected = is_connected; }

    // The generation of this node's child list in which its element children's child indices hold. A new one begins
    // whenever an element child moves relative to the element children after it.
    u32 child_index_generation() const { return m_child_index_generation; }
    bool is_tracked_by_style_engine() const;

    // Whether this subtree waits to take its place in the style engine's tree, and whether a shadow-including
    // descendant's subtree does. See CSS::take_in_pending_style_arrivals().
    bool style_arrival_pending() const { return m_style_arrival_pending; }
    void set_style_arrival_pending(bool value) { m_style_arrival_pending = value; }
    bool descendant_style_arrival_pending() const { return m_descendant_style_arrival_pending; }
    void set_descendant_style_arrival_pending(bool value) { m_descendant_style_arrival_pending = value; }

    // Mirrors the slottable's assigned slot; see SlottableMixin::set_assigned_slot().
    bool has_assigned_slot() const { return m_has_assigned_slot; }
    void set_has_assigned_slot(Badge<SlottableMixin>, bool value) { m_has_assigned_slot = value; }

    bool inside_blocking_wheel_event_handler() const { return m_inside_blocking_wheel_event_handler; }
    bool update_inside_blocking_wheel_event_handler_state();
    void update_inside_blocking_wheel_event_handler_state_for_subtree();

    [[nodiscard]] bool is_browsing_context_connected() const;

    Node* parent_node() { return parent(); }
    Node const* parent_node() const { return parent(); }

    static constexpr size_t parent_node_offset() { return TreeNode<Node>::parent_offset<Node>(); }
    static constexpr size_t first_child_offset() { return TreeNode<Node>::first_child_offset<Node>(); }
    static constexpr size_t last_child_offset() { return TreeNode<Node>::last_child_offset<Node>(); }
    static constexpr size_t previous_sibling_offset() { return TreeNode<Node>::previous_sibling_offset<Node>(); }
    static constexpr size_t next_sibling_offset() { return TreeNode<Node>::next_sibling_offset<Node>(); }

    GC::Ptr<Element> parent_element();
    GC::Ptr<Element const> parent_element() const;

    MUST_UPCALL virtual void inserted();
    virtual void post_connection();
    enum class IsSubtreeRoot : u8 {
        No,
        Yes,
    };
    MUST_UPCALL virtual void removed_from(IsSubtreeRoot, Node* old_ancestor, Node& old_root);
    MUST_UPCALL virtual void moved_from(IsSubtreeRoot, GC::Ptr<Node> old_ancestor);

    struct ChildrenChangedMetadata {
        enum class Type {
            Inserted,
            Removal,
            AllChildrenRemoved,
            Mutation,
        };
        enum class AffectsElements {
            No,
            Yes,
        };
        Type type {};
        GC::Ref<Node> node;
        AffectsElements affects_elements { AffectsElements::No };
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
    [[nodiscard]] bool has_layout_box() const { return m_has_layout_box; }
    [[nodiscard]] bool is_rendered() const { return m_has_committed_box; }
    void set_box_presence(Badge<Layout::NodeArena>, bool has_layout_box, bool has_committed_box)
    {
        m_has_layout_box = has_layout_box;
        m_has_committed_box = has_committed_box;
    }
    Element const* first_letter_owner_for_layout_subtree_from(Layout::BegunRead const& read, Node const& inclusive_ancestor) const;
    Element* first_letter_owner_for_layout_subtree_from(Layout::BegunRead const& read, Node const& inclusive_ancestor)
    {
        return const_cast<Element*>(const_cast<Node const*>(this)->first_letter_owner_for_layout_subtree_from(read, inclusive_ancestor));
    }

    void set_needs_repaint(InvalidateDisplayList = InvalidateDisplayList::PaintCommandsAndHitTestList);
    // The facts about this node that a box built for it paints (inertness, editability, and so on) may have changed.
    void publish_dom_paint_facts();
    void set_needs_layout_update(SetNeedsLayoutReason);
    void set_needs_layout_update(SetNeedsLayoutReason, Layout::LayoutUpdatePropagation);

    // Whether the node's layout subtree can leave the parent's box without restructuring the
    // anonymous boxes around it, so the parent's subtree keeps its layout tree.
    static bool can_detach_layout_subtree_in_place(Layout::BegunRead const& read, Element const& element, Element const& parent, bool box_is_block_level);
    // Whether a list item's box appearing or disappearing changes the list-item counter value of
    // some item that stays in the list.
    static bool list_item_box_change_renumbers_list(Element const& list_item);

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

    [[nodiscard]] bool needs_pseudo_element_layout_tree_update() const { return layout_tree_update_reuse_reasons() & PseudoElementChange; }
    [[nodiscard]] bool may_reuse_layout_node_for_child_list_insertion() const { return layout_tree_update_reuse_reasons() & ChildListInsertion; }

    [[nodiscard]] bool child_needs_layout_tree_update() const;
    void set_child_needs_layout_tree_update(bool);

    // The number of animations associated with this node's shadow-including inclusive subtree. A
    // synchronous read of layout geometry has to catch up the style of a throttled animation that
    // could change what it sees, and this count answers "there is none in here" for a whole subtree
    // at once, so a read far away from the page's animations never looks at an animation.
    [[nodiscard]] u32 associated_animation_count_in_subtree() const { return m_associated_animation_count_in_subtree; }
    void change_associated_animation_count_in_subtree(i32 delta);

    [[nodiscard]] u32 children_explicitly_inherited_non_inherited_style_groups() const { return m_children_explicitly_inherited_non_inherited_style_groups; }
    void add_children_explicitly_inherited_non_inherited_style_groups(u32 style_groups);
    // Tell the style engine of this node's mark, which it keeps for the node's style node.
    void publish_children_explicitly_inherit_mark();

    void record_style_environment_change();
    CSS::StyleScope& style_scope();
    CSS::StyleScope const& style_scope() const { return const_cast<Node*>(this)->style_scope(); }

    void set_document(Badge<Document, NamedNodeMap>, Document&);

    virtual EventTarget* get_parent(Event const&) override;

    template<typename T>
    bool fast_is() const = delete;

    template<typename T>
    T* fast_as() = delete;
    template<typename T>
    T const* fast_as() const = delete;

    WebIDL::ExceptionOr<void> ensure_pre_insertion_validity(GC::Ref<Node> node, GC::Ptr<Node> child, bool exclude_all_children = false) const;

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
    // The node's unique id, without giving it one if it has none.
    [[nodiscard]] Optional<UniqueNodeID> unique_id_if_assigned() const;
    static Node* from_unique_id(UniqueNodeID);

    Optional<String> webdriver_node_id() const;
    void set_webdriver_node_id(String) const;

    WebIDL::ExceptionOr<Utf16String> serialize_fragment(HTML::RequireWellFormed, FragmentSerializationMode = FragmentSerializationMode::Inner) const;

    WebIDL::ExceptionOr<void> unsafely_set_html(Variant<GC::Ref<Element>, GC::Ref<DocumentFragment>>, Utf16View);

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
    bool has_inclusive_ancestor_with_event_listener(Utf16FlyString const& type) const;

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

        GC::Ptr<NodeList> child_nodes;
        GC::Ptr<HTMLCollection> children;
        OwnPtr<HTMLCollectionCacheRegistration::List> html_collections_with_valid_caches;
    };

    void register_html_collection_with_valid_cache(HTMLCollection&);
    void invalidate_html_collection_caches_in_ancestors(ChildrenChangedMetadata::AffectsElements);
    void invalidate_html_collection_caches_in_ancestors_for_attribute_change(HTMLCollectionCacheRegistration::AttributeInvalidationTypes);

    Node(Document&, NodeType);

    void set_document(Document&);

    virtual OwnPtr<RareData> create_rare_data() const;
    RareData& ensure_rare_data() const;
    RareData* rare_data() { return m_rare_data; }
    RareData const* rare_data() const { return m_rare_data; }

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;
    virtual size_t external_memory_size() const override;

    GC::Ptr<Document> m_document;
    GC::Ptr<Node> m_root;
    NodeType m_type { NodeType::INVALID };
    bool m_has_layout_box { false };
    bool m_has_committed_box { false };
    // Which narrower rebuilds the layout tree update marks collected so far still permit.
    enum LayoutTreeUpdateReuseReason : u8 {
        ChildListInsertion = 1,
        PseudoElementChange = 2,
    };
    [[nodiscard]] u8 layout_tree_update_reuse_reasons() const;

    u32 m_children_explicitly_inherited_non_inherited_style_groups { 0 };
    u32 m_associated_animation_count_in_subtree { 0 };
    bool m_in_editable_subtree { false };
    bool m_is_connected { false };
    // NB: These share a byte, which keeps every node from growing.
    bool m_has_assigned_slot : 1 { false };
    bool m_inside_blocking_wheel_event_handler : 1 { false };
    bool m_style_arrival_pending : 1 { false };
    bool m_descendant_style_arrival_pending : 1 { false };
    u32 m_child_index_generation { 1 };
    // The slot of the node directory that names the node by its unique id, or 0 before anything asked for the id.
    mutable u32 m_node_directory_slot { 0 };

    void build_accessibility_tree(AccessibilityTreeNode& parent);

    ErrorOr<Utf16String> name_or_description(Layout::BegunRead const& read, NameOrDescription, Document const&, HashTable<UniqueNodeID>&, IsDescendant = IsDescendant::No, ShouldComputeRole = ShouldComputeRole::Yes) const;

private:
    enum class LayoutSubtreeRemoval {
        DetachInPlace,
        RebuildParent,
    };
    enum class AncestorsMayHaveFirstLetter {
        No,
        Yes,
    };

    void run_node_iterator_pre_removing_steps();
    bool schedule_list_item_renumber_for_removal();
    void report_removal_to_style_engine(Node& parent);
    void update_layout_tree_for_removal(Layout::BegunRead const& read, Node& parent, LayoutSubtreeRemoval, AncestorsMayHaveFirstLetter);
    void assign_slottables_after_removal(Node& parent, Node& parent_root);
    void run_removing_steps(Node& parent, Node& parent_root, bool was_tracked_by_style_engine);
    void add_transient_registered_observers_for_removal(Node& parent);
    void queue_tree_mutation_record_for_removal(Node& parent, GC::Ptr<Node> old_previous_sibling, GC::Ptr<Node> old_next_sibling);

    void queue_tree_mutation_record(ReadonlySpan<GC::Ref<Node>> added_nodes, ReadonlySpan<GC::Ref<Node>> removed_nodes, Node* previous_sibling, Node* next_sibling);

    void live_range_pre_remove();
    void live_range_pre_remove_all_children();

    void insert_before_impl(GC::Ref<Node>, GC::Ptr<Node> child);
    void adjust_live_ranges_for_insertion(Node& child, size_t count);
    void insert_node_into_children(GC::Ref<Node>, GC::Ptr<Node> child);
    void insert_nodes_before(ReadonlySpan<GC::Ref<Node>>, GC::Ptr<Node> child, bool suppress_observers, GC::Ref<Node> metadata_node, ChildrenChangedMetadata::AffectsElements);
    void append_child_impl(GC::Ref<Node>);
    void remove_child_impl(GC::Ref<Node>);
    void begin_child_index_generation();
    void set_root_for_subtree(Node&);

    static Optional<Utf16View> first_valid_id(Utf16View, Document const&);

    mutable OwnPtr<RareData> m_rare_data;
};

}
