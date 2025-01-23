/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/NavigableContainer.h>

namespace Web::HTML {

// NOTE: This element is marked as obsolete, but is still listed as required by the specification.
class HTMLFrameElement final : public NavigableContainer {
    WEB_PLATFORM_OBJECT(HTMLFrameElement, NavigableContainer);
    GC_DECLARE_ALLOCATOR(HTMLFrameElement);

public:
    virtual ~HTMLFrameElement() override;

private:
    HTMLFrameElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    // ^DOM::Element
    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;
    virtual void adjust_computed_style(CSS::ComputedProperties&) override;

    void process_the_frame_attributes(bool initial_insertion = false);
};

}
