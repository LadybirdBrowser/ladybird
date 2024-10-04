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

JS_DEFINE_ALLOCATOR(Comment);

Comment::Comment(Document& document, String const& data)
    : CharacterData(document, NodeType::COMMENT_NODE, data)
{
}

// https://dom.spec.whatwg.org/#dom-comment-comment
WebIDL::ExceptionOr<JS::NonnullGCPtr<Comment>> Comment::construct_impl(JS::Realm& realm, String const& data)
{
    auto& window = verify_cast<HTML::Window>(realm.global_object());
    return realm.heap().allocate<Comment>(realm, window.associated_document(), data);
}

void Comment::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Comment);
}

}
