/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Script.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#classic-script
class ClassicScript final : public Script {
    JS_CELL(ClassicScript, Script);
    JS_DECLARE_ALLOCATOR(ClassicScript);

public:
    virtual ~ClassicScript() override;

    enum class MutedErrors {
        No,
        Yes,
    };
    static JS::NonnullGCPtr<ClassicScript> create(ByteString filename, StringView source, EnvironmentSettingsObject&, URL::URL base_url, size_t source_line_number = 1, MutedErrors = MutedErrors::No);

    JS::Script* script_record() { return m_script_record; }
    JS::Script const* script_record() const { return m_script_record; }

    enum class RethrowErrors {
        No,
        Yes,
    };
    JS::Completion run(RethrowErrors = RethrowErrors::No);

    MutedErrors muted_errors() const { return m_muted_errors; }

private:
    ClassicScript(URL::URL base_url, ByteString filename, EnvironmentSettingsObject& environment_settings_object);

    virtual void visit_edges(Cell::Visitor&) override;

    JS::GCPtr<JS::Script> m_script_record;
    MutedErrors m_muted_errors { MutedErrors::No };
};

}
