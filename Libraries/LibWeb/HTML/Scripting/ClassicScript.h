/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Script.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#classic-script
class WEB_API ClassicScript final : public Script {
    GC_CELL(ClassicScript, Script);
    GC_DECLARE_ALLOCATOR(ClassicScript);

public:
    virtual ~ClassicScript() override;

    enum class MutedErrors {
        No,
        Yes,
    };
    static GC::Ref<ClassicScript> create(ByteString filename, StringView source, JS::Realm&, URL::URL base_url, size_t source_line_number = 1, MutedErrors = MutedErrors::No);

    JS::Script* script_record() { return m_script_record; }
    JS::Script const* script_record() const { return m_script_record; }

    enum class RethrowErrors {
        No,
        Yes,
    };
    JS::Completion run(RethrowErrors = RethrowErrors::No, GC::Ptr<JS::Environment> lexical_environment_override = {});

    MutedErrors muted_errors() const { return m_muted_errors; }

private:
    ClassicScript(URL::URL base_url, ByteString filename, JS::Realm&);

    virtual bool is_classic_script() const final { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<JS::Script> m_script_record;
    MutedErrors m_muted_errors { MutedErrors::No };
};

}

template<>
inline bool JS::Script::HostDefined::fast_is<Web::HTML::ClassicScript>() const { return is_classic_script(); }
