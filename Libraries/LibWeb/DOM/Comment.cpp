/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Comment);

Comment::Comment(Document& document, Utf16String data)
    : CharacterData(document, NodeType::COMMENT_NODE, move(data))
{
}

GC::Ref<Comment> Comment::create(Document& document, Utf16String data)
{
    return GC::Heap::the().allocate<Comment>(document, move(data));
}

GC::Ref<Comment> Comment::construct_impl(JS::Realm& realm, Utf16String data)
{
    return create(HTML::relevant_window(realm.global_object()).associated_document(), move(data));
}

}
