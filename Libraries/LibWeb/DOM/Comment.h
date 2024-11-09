/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/CharacterData.h>

namespace Web::DOM {

class Comment final : public CharacterData {
    WEB_PLATFORM_OBJECT(Comment, CharacterData);
    JS_DECLARE_ALLOCATOR(Comment);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Comment>> construct_impl(JS::Realm&, String const& data);
    virtual ~Comment() override = default;

    virtual FlyString node_name() const override { return "#comment"_fly_string; }

private:
    Comment(Document&, String const&);

    virtual void initialize(JS::Realm&) override;
};

template<>
inline bool Node::fast_is<Comment>() const { return is_comment(); }

}
