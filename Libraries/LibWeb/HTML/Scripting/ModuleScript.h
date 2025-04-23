/*
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/SourceTextModule.h>
#include <LibWeb/HTML/Scripting/Script.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#module-script
class ModuleScript : public Script {
    GC_CELL(ModuleScript, Script);

public:
    virtual ~ModuleScript() override;

protected:
    ModuleScript(URL::URL base_url, ByteString filename, JS::Realm&);

private:
    virtual bool is_module_script() const final { return true; }
};

class JavaScriptModuleScript final : public ModuleScript {
    GC_CELL(JavaScriptModuleScript, ModuleScript);
    GC_DECLARE_ALLOCATOR(JavaScriptModuleScript);

public:
    virtual ~JavaScriptModuleScript() override;

    static WebIDL::ExceptionOr<GC::Ptr<JavaScriptModuleScript>> create(ByteString const& filename, StringView source, JS::Realm&, URL::URL base_url);

    enum class PreventErrorReporting {
        Yes,
        No
    };

    JS::Promise* run(PreventErrorReporting = PreventErrorReporting::No);

    JS::SourceTextModule const* record() const { return m_record.ptr(); }
    JS::SourceTextModule* record() { return m_record.ptr(); }

protected:
    JavaScriptModuleScript(URL::URL base_url, ByteString filename, JS::Realm&);

private:
    virtual bool is_javascript_module_script() const final { return true; }
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ptr<JS::SourceTextModule> m_record;

    size_t m_fetch_internal_request_count { 0 };
    size_t m_completed_fetch_internal_request_count { 0 };

    Function<void(JavaScriptModuleScript const*)> m_completed_fetch_internal_callback;
};

}

template<>
inline bool JS::Script::HostDefined::fast_is<Web::HTML::ModuleScript>() const { return is_module_script(); }

template<>
inline bool JS::Script::HostDefined::fast_is<Web::HTML::JavaScriptModuleScript>() const { return is_javascript_module_script(); }
