/*
 * Copyright (c) 2025, the Ladybird Browser contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/MathML/MathMLElement.h>

namespace Web::MathML {

class MathMLStringElement final : public MathMLElement {
    WEB_PLATFORM_OBJECT(MathMLStringElement, MathMLElement);
    GC_DECLARE_ALLOCATOR(MathMLStringElement);

public:
    virtual ~MathMLStringElement() override = default;

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;
    virtual void children_changed(DOM::Node::ChildrenChangedMetadata const*) override;
    virtual void attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual void inserted() override;

private:
    MathMLStringElement(DOM::Document&, DOM::QualifiedName);

    void ensure_quotes();
    String resolved_left_quote() const;
    String resolved_right_quote() const;

    bool m_is_generating_quotes { false };
    DOM::Text* m_left_quote_text_node { nullptr };
    DOM::Text* m_right_quote_text_node { nullptr };
};

}
