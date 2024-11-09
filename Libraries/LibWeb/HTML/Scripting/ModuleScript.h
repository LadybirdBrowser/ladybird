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
    JS_CELL(ModuleScript, Script);

public:
    virtual ~ModuleScript() override;

protected:
    ModuleScript(URL::URL base_url, ByteString filename, JS::Realm&);
};

class JavaScriptModuleScript final : public ModuleScript {
    JS_CELL(JavaScriptModuleScript, ModuleScript);
    JS_DECLARE_ALLOCATOR(JavaScriptModuleScript);

public:
    virtual ~JavaScriptModuleScript() override;

    static WebIDL::ExceptionOr<JS::GCPtr<JavaScriptModuleScript>> create(ByteString const& filename, StringView source, JS::Realm&, URL::URL base_url);

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
    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::GCPtr<JS::SourceTextModule> m_record;

    size_t m_fetch_internal_request_count { 0 };
    size_t m_completed_fetch_internal_request_count { 0 };

    Function<void(JavaScriptModuleScript const*)> m_completed_fetch_internal_callback;
};

}
