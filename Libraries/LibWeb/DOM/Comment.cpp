/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CommentPrototype.h>
#include <LibWeb/DOM/Comment.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(Comment);

Comment::Comment(Document& document, Utf16String data)
    : CharacterData(document, NodeType::COMMENT_NODE, move(data))
{
}

// https://dom.spec.whatwg.org/#dom-comment-comment
WebIDL::ExceptionOr<GC::Ref<Comment>> Comment::construct_impl(JS::Realm& realm, Utf16String data)
{
    auto& window = as<HTML::Window>(realm.global_object());
    return realm.create<Comment>(window.associated_document(), move(data));
}

void Comment::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Comment);
    Base::initialize(realm);
}

}
