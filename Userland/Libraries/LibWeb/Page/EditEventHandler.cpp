/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/EditEventHandler.h>

namespace Web {

void EditEventHandler::handle_delete_character_after(JS::NonnullGCPtr<DOM::Document> document, JS::NonnullGCPtr<DOM::Position> cursor_position)
{
    auto& node = verify_cast<DOM::Text>(*cursor_position->node());
    auto& text = node.data();

    auto next_offset = node.segmenter().next_boundary(cursor_position->offset());
    if (!next_offset.has_value()) {
        // FIXME: Move to the next node and delete the first character there.
        return;
    }

    StringBuilder builder;
    builder.append(text.bytes_as_string_view().substring_view(0, cursor_position->offset()));
    builder.append(text.bytes_as_string_view().substring_view(*next_offset));
    node.set_data(MUST(builder.to_string()));

    document->user_did_edit_document_text({});
}

// This method is quite convoluted but this is necessary to make editing feel intuitive.
void EditEventHandler::handle_delete(JS::NonnullGCPtr<DOM::Document> document, DOM::Range& range)
{
    auto* start = verify_cast<DOM::Text>(range.start_container());
    auto* end = verify_cast<DOM::Text>(range.end_container());

    if (start == end) {
        StringBuilder builder;
        builder.append(start->data().bytes_as_string_view().substring_view(0, range.start_offset()));
        builder.append(end->data().bytes_as_string_view().substring_view(range.end_offset()));

        start->set_data(MUST(builder.to_string()));
    } else {
        // Remove all the nodes that are fully enclosed in the range.
        HashTable<DOM::Node*> queued_for_deletion;
        for (auto* node = start->next_in_pre_order(); node; node = node->next_in_pre_order()) {
            if (node == end)
                break;

            queued_for_deletion.set(node);
        }
        for (auto* parent = start->parent(); parent; parent = parent->parent())
            queued_for_deletion.remove(parent);
        for (auto* parent = end->parent(); parent; parent = parent->parent())
            queued_for_deletion.remove(parent);
        for (auto* node : queued_for_deletion)
            node->remove();

        // Join the parent nodes of start and end.
        DOM::Node *insert_after = start, *remove_from = end, *parent_of_end = end->parent();
        while (remove_from) {
            auto* next_sibling = remove_from->next_sibling();

            remove_from->remove();
            insert_after->parent()->insert_before(*remove_from, *insert_after);

            insert_after = remove_from;
            remove_from = next_sibling;
        }
        if (!parent_of_end->has_children()) {
            if (parent_of_end->parent())
                parent_of_end->remove();
        }

        // Join the start and end nodes.
        StringBuilder builder;
        builder.append(start->data().bytes_as_string_view().substring_view(0, range.start_offset()));
        builder.append(end->data().bytes_as_string_view().substring_view(range.end_offset()));

        start->set_data(MUST(builder.to_string()));
        end->remove();
    }

    document->user_did_edit_document_text({});
}

void EditEventHandler::handle_insert(JS::NonnullGCPtr<DOM::Document> document, JS::NonnullGCPtr<DOM::Position> position, u32 code_point)
{
    StringBuilder builder;
    builder.append_code_point(code_point);
    handle_insert(document, position, MUST(builder.to_string()));
}

void EditEventHandler::handle_insert(JS::NonnullGCPtr<DOM::Document> document, JS::NonnullGCPtr<DOM::Position> position, String data)
{
    if (is<DOM::Text>(*position->node())) {
        auto& node = verify_cast<DOM::Text>(*position->node());

        StringBuilder builder;
        builder.append(node.data().bytes_as_string_view().substring_view(0, position->offset()));
        builder.append(data);
        builder.append(node.data().bytes_as_string_view().substring_view(position->offset()));

        // Cut string by max length
        // FIXME: Cut by UTF-16 code units instead of raw bytes
        if (auto max_length = node.max_length(); max_length.has_value() && builder.string_view().length() > *max_length) {
            node.set_data(MUST(String::from_utf8(builder.string_view().substring_view(0, *max_length))));
        } else {
            node.set_data(MUST(builder.to_string()));
        }
        node.invalidate_style();
    } else {
        auto& node = *position->node();
        auto& realm = node.realm();
        auto text = realm.heap().allocate<DOM::Text>(realm, node.document(), data);
        MUST(node.append_child(*text));
        position->set_node(text);
        position->set_offset(1);
    }

    document->user_did_edit_document_text({});
}

}
