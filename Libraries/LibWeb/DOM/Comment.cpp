/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Comment.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

namespace Web::DOM {

#if ARCH(X86_64) && !defined(AK_OS_WINDOWS)
static_assert(sizeof(Comment) == 104);
#endif

static_assert(is_shell_of_rust_object<Comment, RustFFI::CommentObject>);

Comment::Comment(VtableProbe probe)
    : CharacterData(probe)
{
}

GC::Ref<Comment> Comment::create(Document& document, Utf16String data)
{
    return static_cast<Comment&>(*RustFFI::rust_dom_create_comment(&document, move(data).into_raw()));
}

GC::Ref<Comment> Comment::create_for_constructor(JS::Object& relevant_global_object, Utf16String data)
{
    return create(HTML::relevant_window(relevant_global_object).associated_document(), move(data));
}

}
