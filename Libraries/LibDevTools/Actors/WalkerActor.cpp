/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/StringUtils.h>
#include <LibDevTools/Actors/LayoutInspectorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWeb/DOM/NodeType.h>
#include <LibWebView/Mutation.h>

namespace DevTools {

NonnullRefPtr<WalkerActor> WalkerActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, JsonObject dom_tree)
{
    return adopt_ref(*new WalkerActor(devtools, move(name), move(tab), move(dom_tree)));
}

WalkerActor::WalkerActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, JsonObject dom_tree)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_dom_tree(move(dom_tree))
{
    populate_dom_tree_cache();

    if (auto tab = m_tab.strong_ref()) {
        devtools.delegate().listen_for_dom_mutations(tab->description(),
            [weak_self = make_weak_ptr<WalkerActor>()](WebView::Mutation mutation) {
                if (auto self = weak_self.strong_ref())
                    self->new_dom_node_mutation(move(mutation));
            });
    }
}

WalkerActor::~WalkerActor()
{
    if (auto tab = m_tab.strong_ref())
        devtools().delegate().stop_listening_for_dom_mutations(tab->description());
}

void WalkerActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "children"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto ancestor_node = WalkerActor::dom_node_for(*this, *node);
        if (!ancestor_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        JsonArray nodes;

        if (auto children = ancestor_node->node.get_array("children"_sv); children.has_value()) {
            children->for_each([&](JsonValue const& child) {
                nodes.must_append(serialize_node(child.as_object()));
            });
        }

        response.set("hasFirst"_sv, !nodes.is_empty());
        response.set("hasLast"_sv, !nodes.is_empty());
        response.set("nodes"_sv, move(nodes));
        send_response(message, move(response));
        return;
    }

    if (message.type == "duplicateNode"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        devtools().delegate().clone_dom_node(dom_node->tab->description(), dom_node->identifier.id, default_async_handler(message));
        return;
    }

    if (message.type == "editTagName"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto tag_name = get_required_parameter<String>(message, "tagName"_sv);
        if (!tag_name.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        devtools().delegate().set_dom_node_tag(dom_node->tab->description(), dom_node->identifier.id, *tag_name, default_async_handler(message));
        return;
    }

    if (message.type == "getLayoutInspector"_sv) {
        if (!m_layout_inspector)
            m_layout_inspector = devtools().register_actor<LayoutInspectorActor>();

        JsonObject actor;
        actor.set("actor"_sv, m_layout_inspector->name());

        response.set("actor"_sv, move(actor));
        send_response(message, move(response));
        return;
    }

    if (message.type == "getMutations"_sv) {
        response.set("mutations"_sv, serialize_mutations());
        send_response(message, move(response));

        m_has_new_mutations_since_last_mutations_request = false;
        return;
    }

    if (message.type == "getOffsetParent"_sv) {
        response.set("node"_sv, JsonValue {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "innerHTML"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        devtools().delegate().get_dom_node_inner_html(dom_node->tab->description(), dom_node->identifier.id,
            async_handler(message, [](auto&, auto html, auto& response) {
                response.set("value"_sv, move(html));
            }));

        return;
    }

    if (message.type == "insertAdjacentHTML") {
        // FIXME: This message also contains `value` and `position` parameters, containing the HTML to insert and the
        //        location to insert it. For the "Create New Node" action, this is always "<div></div>" and "beforeEnd",
        //        which is exactly what our WebView implementation currently supports.
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        devtools().delegate().create_child_element(dom_node->tab->description(), dom_node->identifier.id,
            async_handler<WalkerActor>(message, [](auto& self, auto node_id, auto& response) {
                JsonArray nodes;

                if (auto actor = self.m_dom_node_id_to_actor_map.get(node_id); actor.has_value()) {
                    if (auto dom_node = WalkerActor::dom_node_for(self, *actor); dom_node.has_value())
                        nodes.must_append(self.serialize_node(dom_node->node));
                }

                response.set("newParents"_sv, JsonArray {});
                response.set("nodes"_sv, move(nodes));
            }));

        return;
    }

    if (message.type == "insertBefore"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto parent = get_required_parameter<String>(message, "parent"_sv);
        if (!parent.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        auto parent_dom_node = WalkerActor::dom_node_for(*this, *parent);
        if (!parent_dom_node.has_value()) {
            send_unknown_actor_error(message, *parent);
            return;
        }

        Optional<Web::UniqueNodeID> sibling_node_id;
        if (auto sibling = message.data.get_string("sibling"_sv); sibling.has_value()) {
            auto sibling_dom_node = WalkerActor::dom_node_for(*this, *sibling);
            if (!sibling_dom_node.has_value()) {
                send_unknown_actor_error(message, *sibling);
                return;
            }

            sibling_node_id = sibling_dom_node->identifier.id;
        }

        devtools().delegate().insert_dom_node_before(dom_node->tab->description(), dom_node->identifier.id, parent_dom_node->identifier.id, sibling_node_id, default_async_handler(message));
        return;
    }

    if (message.type == "isInDOMTree"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        response.set("attached"_sv, m_actor_to_dom_node_map.contains(*node));
        send_response(message, move(response));
        return;
    }

    if (message.type == "outerHTML"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        devtools().delegate().get_dom_node_outer_html(dom_node->tab->description(), dom_node->identifier.id,
            async_handler(message, [](auto&, auto html, auto& response) {
                response.set("value"_sv, move(html));
            }));

        return;
    }

    if (message.type == "previousSibling"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        JsonValue previous_sibling;
        if (auto previous_sibling_node = previous_sibling_for_node(dom_node->node); previous_sibling_node.has_value())
            previous_sibling = serialize_node(*previous_sibling_node);

        response.set("node"_sv, move(previous_sibling));
        send_response(message, move(response));
        return;
    }

    if (message.type == "querySelector"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto selector = get_required_parameter<String>(message, "selector"_sv);
        if (!selector.has_value())
            return;

        auto ancestor_node = WalkerActor::dom_node_for(*this, *node);
        if (!ancestor_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        if (auto selected_node = find_node_by_selector(ancestor_node->node, *selector); selected_node.has_value()) {
            response.set("node"_sv, serialize_node(*selected_node));

            if (auto parent = m_dom_node_to_parent_map.get(&selected_node.value()); parent.value() && parent.value() != &ancestor_node->node) {
                // FIXME: Should this be a stack of nodes leading to `ancestor_node`?
                JsonArray new_parents;
                new_parents.must_append(serialize_node(*parent.value()));

                response.set("newParents"_sv, move(new_parents));
            }
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "removeNode"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        JsonValue next_sibling;
        if (auto next_sibling_node = next_sibling_for_node(dom_node->node); next_sibling_node.has_value())
            next_sibling = serialize_node(*next_sibling_node);

        auto parent_node = remove_node(dom_node->node);
        if (!parent_node.has_value())
            return;

        devtools().delegate().remove_dom_node(dom_node->tab->description(), dom_node->identifier.id,
            async_handler(message, [next_sibling = move(next_sibling)](auto&, auto, auto& response) mutable {
                response.set("nextSibling"_sv, move(next_sibling));
            }));

        return;
    }

    if (message.type == "retainNode"_sv) {
        send_response(message, move(response));
        return;
    }

    if (message.type == "setOuterHTML"_sv) {
        auto node = get_required_parameter<String>(message, "node"_sv);
        if (!node.has_value())
            return;

        auto value = get_required_parameter<String>(message, "value"_sv);
        if (!value.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(*this, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        devtools().delegate().set_dom_node_outer_html(dom_node->tab->description(), dom_node->identifier.id, value.release_value(), default_async_handler(message));
        return;
    }

    if (message.type == "watchRootNode"_sv) {
        response.set("type"_sv, "root-available"_sv);
        response.set("node"_sv, serialize_root());
        send_response(message, move(response));

        send_message({});
        return;
    }

    send_unrecognized_packet_type_error(message);
}

bool WalkerActor::is_suitable_for_dom_inspection(JsonValue const& node)
{
    if (!node.is_object())
        return true;

    auto const& object = node.as_object();

    if (!object.has_string("name"_sv) || !object.has_string("type"_sv))
        return false;

    if (auto text = object.get_string("text"_sv); text.has_value()) {
        if (AK::StringUtils::is_whitespace(*text))
            return false;
    }
    if (auto data = object.get_string("data"_sv); data.has_value()) {
        if (AK::StringUtils::is_whitespace(*data))
            return false;
    }

    return true;
}

JsonValue WalkerActor::serialize_root() const
{
    return serialize_node(m_dom_tree);
}

JsonValue WalkerActor::serialize_node(JsonObject const& node) const
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto actor = node.get_string("actor"_sv);
    if (!actor.has_value())
        return {};

    auto name = node.get_string("name"_sv).release_value();
    auto type = node.get_string("type"_sv).release_value();

    auto dom_type = Web::DOM::NodeType::INVALID;
    JsonValue node_value;

    auto is_top_level_document = &node == &m_dom_tree;
    auto is_displayed = !is_top_level_document && node.get_bool("visible"_sv).value_or(false);
    auto is_scrollable = node.get_bool("scrollable"_sv).value_or(false);
    auto is_shadow_root = false;

    if (type == "document"_sv) {
        dom_type = Web::DOM::NodeType::DOCUMENT_NODE;
    } else if (type == "element"_sv) {
        dom_type = Web::DOM::NodeType::ELEMENT_NODE;
    } else if (type == "text"_sv) {
        dom_type = Web::DOM::NodeType::TEXT_NODE;

        if (auto text = node.get_string("text"_sv); text.has_value())
            node_value = text.release_value();
    } else if (type == "comment"_sv) {
        dom_type = Web::DOM::NodeType::COMMENT_NODE;

        if (auto data = node.get_string("data"_sv); data.has_value())
            node_value = data.release_value();
    } else if (type == "shadow-root"_sv) {
        is_shadow_root = true;
    }

    size_t child_count = 0;
    if (auto children = node.get_array("children"_sv); children.has_value())
        child_count = children->size();

    JsonArray attrs;

    if (auto attributes = node.get_object("attributes"_sv); attributes.has_value()) {
        attributes->for_each_member([&](String const& name, JsonValue const& value) {
            if (!value.is_string())
                return;

            JsonObject attr;
            attr.set("name"_sv, name);
            attr.set("value"_sv, value.as_string());
            attrs.must_append(move(attr));
        });
    }

    JsonObject serialized;
    serialized.set("actor"_sv, actor.release_value());
    serialized.set("attrs"_sv, move(attrs));
    serialized.set("baseURI"_sv, tab->description().url);
    serialized.set("causesOverflow"_sv, false);
    serialized.set("containerType"_sv, JsonValue {});
    serialized.set("displayName"_sv, name.to_ascii_lowercase());
    serialized.set("displayType"_sv, "block"_sv);
    serialized.set("hasEventListeners"_sv, false);
    serialized.set("isAfterPseudoElement"_sv, false);
    serialized.set("isAnonymous"_sv, false);
    serialized.set("isBeforePseudoElement"_sv, false);
    serialized.set("isDirectShadowHostChild"_sv, JsonValue {});
    serialized.set("isDisplayed"_sv, is_displayed);
    serialized.set("isInHTMLDocument"_sv, true);
    serialized.set("isMarkerPseudoElement"_sv, false);
    serialized.set("isNativeAnonymous"_sv, false);
    serialized.set("isScrollable"_sv, is_scrollable);
    serialized.set("isShadowHost"_sv, false);
    serialized.set("isShadowRoot"_sv, is_shadow_root);
    serialized.set("isTopLevelDocument"_sv, is_top_level_document);
    serialized.set("nodeName"_sv, name);
    serialized.set("nodeType"_sv, to_underlying(dom_type));
    serialized.set("nodeValue"_sv, move(node_value));
    serialized.set("numChildren"_sv, child_count);
    serialized.set("shadowRootMode"_sv, JsonValue {});
    serialized.set("traits"_sv, JsonObject {});

    // FIXME: De-duplicate this string. LibDevTools currently cannot depend on LibWeb.
    serialized.set("namespaceURI"_sv, "http://www.w3.org/1999/xhtml"_sv);

    if (!is_top_level_document) {
        if (auto parent = m_dom_node_to_parent_map.get(&node); parent.has_value() && parent.value()) {
            actor = parent.value()->get_string("actor"_sv);
            if (!actor.has_value())
                return {};

            serialized.set("parent"_sv, actor.release_value());
        }
    }

    return serialized;
}

Optional<WalkerActor::DOMNode> WalkerActor::dom_node_for(WeakPtr<WalkerActor> const& weak_walker, StringView actor)
{
    if (auto walker = weak_walker.strong_ref())
        return walker->dom_node(actor);
    return {};
}

Optional<WalkerActor::DOMNode> WalkerActor::dom_node(StringView actor)
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto maybe_dom_node = m_actor_to_dom_node_map.get(actor);
    if (!maybe_dom_node.has_value() || !maybe_dom_node.value())
        return {};

    auto const& dom_node = *maybe_dom_node.value();
    auto identifier = NodeIdentifier::for_node(dom_node);

    return DOMNode { .node = dom_node, .identifier = move(identifier), .tab = tab.release_nonnull() };
}

Optional<JsonObject const&> WalkerActor::find_node_by_selector(JsonObject const& node, StringView selector)
{
    auto matches = [&](auto const& candidate) {
        return candidate.get_string("name"_sv)->equals_ignoring_ascii_case(selector);
    };

    if (matches(node))
        return node;

    if (auto children = node.get_array("children"_sv); children.has_value()) {
        for (size_t i = 0; i < children->size(); ++i) {
            auto const& child = children->at(i);

            if (matches(child.as_object()))
                return child.as_object();

            if (auto result = find_node_by_selector(child.as_object(), selector); result.has_value())
                return result;
        }
    }

    return {};
}

enum class Direction {
    Previous,
    Next,
};
static Optional<JsonObject const&> sibling_for_node(JsonObject const& parent, JsonObject const& node, Direction direction)
{
    auto children = parent.get_array("children"_sv);
    VERIFY(children.has_value());

    auto index = children->values().find_first_index_if([&](auto const& child) {
        return &child.as_object() == &node;
    });
    VERIFY(index.has_value());

    switch (direction) {
    case Direction::Previous:
        if (*index == 0)
            return {};
        index = *index - 1;
        break;

    case Direction::Next:
        if (*index == children->size() - 1)
            return {};
        index = *index + 1;
        break;
    }

    return children->at(*index).as_object();
}

Optional<JsonObject const&> WalkerActor::previous_sibling_for_node(JsonObject const& node)
{
    auto parent = m_dom_node_to_parent_map.get(&node);
    if (!parent.has_value() || !parent.value())
        return {};
    return sibling_for_node(*parent.value(), node, Direction::Previous);
}

Optional<JsonObject const&> WalkerActor::next_sibling_for_node(JsonObject const& node)
{
    auto parent = m_dom_node_to_parent_map.get(&node);
    if (!parent.has_value() || !parent.value())
        return {};
    return sibling_for_node(*parent.value(), node, Direction::Next);
}

Optional<JsonObject const&> WalkerActor::remove_node(JsonObject const& node)
{
    auto maybe_parent = m_dom_node_to_parent_map.get(&node);
    if (!maybe_parent.has_value() || !maybe_parent.value())
        return {};
    auto const& parent = *maybe_parent.value();

    auto children = parent.get_array("children"_sv);
    VERIFY(children.has_value());

    const_cast<JsonArray&>(*children).values().remove_first_matching([&](auto const& child) {
        return &child.as_object() == &node;
    });

    populate_dom_tree_cache();
    return parent;
}

void WalkerActor::new_dom_node_mutation(WebView::Mutation mutation)
{
    auto serialized_target = JsonValue::from_string(mutation.serialized_target);
    if (serialized_target.is_error() || !serialized_target.value().is_object()) {
        dbgln_if(DEVTOOLS_DEBUG, "Unable to parse serialized target as JSON object: {}", serialized_target.error());
        return;
    }

    if (!replace_node_in_tree(move(serialized_target.release_value().as_object()))) {
        dbgln_if(DEVTOOLS_DEBUG, "Unable to apply mutation to DOM tree");
        return;
    }

    m_dom_node_mutations.append(move(mutation));

    if (m_has_new_mutations_since_last_mutations_request)
        return;

    JsonObject message;
    message.set("type"_sv, "newMutations"_sv);
    send_message(move(message));

    m_has_new_mutations_since_last_mutations_request = true;
}

JsonValue WalkerActor::serialize_mutations()
{
    JsonArray mutations;
    mutations.ensure_capacity(m_dom_node_mutations.size());

    for (auto& mutation : m_dom_node_mutations) {
        auto target = m_dom_node_id_to_actor_map.get(mutation.target);
        if (!target.has_value())
            continue;

        JsonObject serialized;
        serialized.set("target"_sv, target.release_value());
        serialized.set("type"_sv, move(mutation.type));

        mutation.mutation.visit(
            [&](WebView::AttributeMutation& mutation) {
                serialized.set("attributeName"_sv, move(mutation.attribute_name));

                if (mutation.new_value.has_value())
                    serialized.set("newValue"_sv, mutation.new_value.release_value());
                else
                    serialized.set("newValue"_sv, JsonValue {});
            },
            [&](WebView::CharacterDataMutation& mutation) {
                serialized.set("newValue"_sv, move(mutation.new_value));
            },
            [&](WebView::ChildListMutation const& mutation) {
                JsonArray added;
                JsonArray removed;

                for (auto id : mutation.added) {
                    if (auto node = m_dom_node_id_to_actor_map.get(id); node.has_value())
                        added.must_append(node.release_value());
                }
                for (auto id : mutation.removed) {
                    if (auto node = m_dom_node_id_to_actor_map.get(id); node.has_value())
                        removed.must_append(node.release_value());
                }

                serialized.set("added"_sv, move(added));
                serialized.set("removed"_sv, move(removed));
                serialized.set("numChildren"_sv, mutation.target_child_count);
            });

        mutations.must_append(move(serialized));
    }

    m_dom_node_mutations.clear();
    return mutations;
}

bool WalkerActor::replace_node_in_tree(JsonObject replacement)
{
    auto const& actor = actor_for_node(replacement);

    auto node = m_actor_to_dom_node_map.get(actor.name());
    if (!node.has_value() || !node.value())
        return false;

    const_cast<JsonObject&>(*node.value()) = move(replacement);
    populate_dom_tree_cache();

    return true;
}

void WalkerActor::populate_dom_tree_cache()
{
    m_dom_node_to_parent_map.clear();
    m_actor_to_dom_node_map.clear();
    m_dom_node_id_to_actor_map.clear();

    populate_dom_tree_cache(m_dom_tree, nullptr);
}

void WalkerActor::populate_dom_tree_cache(JsonObject& node, JsonObject const* parent)
{
    auto const& node_actor = actor_for_node(node);
    node.set("actor"_sv, node_actor.name());

    m_dom_node_to_parent_map.set(&node, parent);
    m_actor_to_dom_node_map.set(node_actor.name(), &node);

    if (!node_actor.node_identifier().pseudo_element.has_value())
        m_dom_node_id_to_actor_map.set(node_actor.node_identifier().id, node_actor.name());

    auto children = node.get_array("children"_sv);
    if (!children.has_value())
        return;

    children->values().remove_all_matching([&](JsonValue const& child) {
        return !is_suitable_for_dom_inspection(child);
    });

    children->for_each([&](JsonValue& child) {
        populate_dom_tree_cache(child.as_object(), &node);
    });
}

NodeActor const& WalkerActor::actor_for_node(JsonObject const& node)
{
    auto identifier = NodeIdentifier::for_node(node);

    if (auto it = m_node_actors.find(identifier); it != m_node_actors.end()) {
        if (auto node_actor = it->value.strong_ref())
            return *node_actor;
    }

    auto& node_actor = devtools().register_actor<NodeActor>(move(identifier), *this);
    m_node_actors.set(identifier, node_actor);

    return node_actor;
}

}
