/*
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/SourceTextModule.h>
#include <LibJS/SyntheticModule.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

// FIXME: Support WebAssembly Module Record
using ModuleScriptRecord = Variant<Empty, GC::Ref<JS::SourceTextModule>, GC::Ref<JS::SyntheticModule>>;

// https://html.spec.whatwg.org/multipage/webappapis.html#module-script
class WEB_API ModuleScript : public Script {
    GC_CELL(ModuleScript, Script);

public:
    virtual ~ModuleScript() override;

    static WebIDL::ExceptionOr<GC::Ptr<ModuleScript>> create_a_javascript_module_script(ByteString const& filename, StringView source, JS::Realm&, URL::URL base_url);
    static WebIDL::ExceptionOr<GC::Ptr<ModuleScript>> create_a_css_module_script(ByteString const& filename, StringView source, JS::Realm&);

    enum class PreventErrorReporting {
        Yes,
        No
    };

    JS::Promise* run(PreventErrorReporting = PreventErrorReporting::No);

    ModuleScriptRecord record() const { return m_record; }

protected:
    ModuleScript(Optional<URL::URL> base_url, ByteString filename, JS::Realm&);

private:
    virtual bool is_module_script() const final { return true; }
    virtual void visit_edges(JS::Cell::Visitor&) override;

    ModuleScriptRecord m_record;

    size_t m_fetch_internal_request_count { 0 };
    size_t m_completed_fetch_internal_request_count { 0 };

    Function<void(ModuleScript const*)> m_completed_fetch_internal_callback;
};

}

template<>
inline bool JS::Script::HostDefined::fast_is<Web::HTML::ModuleScript>() const { return is_module_script(); }
