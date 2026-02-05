/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/FrontendAction.h>

struct LibJSCellMacro {
    enum class Type {
        GCCell,
        ForeignCell,
        JSObject,
        JSEnvironment,
        JSPrototypeObject,
        WebPlatformObject,
    };

    struct Arg {
        std::string text;
        clang::SourceLocation location;
    };

    clang::SourceRange range;
    Type type;
    std::vector<Arg> args;

    static char const* type_name(Type);
};

using LibJSCellMacroMap = std::unordered_map<unsigned int, std::vector<LibJSCellMacro>>;

class LibJSPPCallbacks : public clang::PPCallbacks {
public:
    LibJSPPCallbacks(clang::Preprocessor& preprocessor, LibJSCellMacroMap& macro_map)
        : m_preprocessor(preprocessor)
        , m_macro_map(macro_map)
    {
    }

    virtual void LexedFileChanged(clang::FileID curr_fid, LexedFileChangeReason, clang::SrcMgr::CharacteristicKind, clang::FileID, clang::SourceLocation) override;

    virtual void MacroExpands(clang::Token const& name_token, clang::MacroDefinition const& definition, clang::SourceRange range, clang::MacroArgs const* args) override;

private:
    clang::Preprocessor& m_preprocessor;
    std::vector<unsigned int> m_curr_fid_hash_stack;
    LibJSCellMacroMap& m_macro_map;
};

class LibJSGCVisitor : public clang::RecursiveASTVisitor<LibJSGCVisitor> {
public:
    explicit LibJSGCVisitor(clang::ASTContext& context, LibJSCellMacroMap const& macro_map, bool detect_invalid_function_members)
        : m_context(context)
        , m_macro_map(macro_map)
        , m_detect_invalid_function_members(detect_invalid_function_members)
    {
    }

    bool VisitCXXRecordDecl(clang::CXXRecordDecl*);

private:
    struct CellMacroExpectation {
        LibJSCellMacro::Type type;
        std::string base_name;
    };

    void validate_record_macros(clang::CXXRecordDecl const&);
    CellMacroExpectation get_record_cell_macro_expectation(clang::CXXRecordDecl const&);

    clang::ASTContext& m_context;
    LibJSCellMacroMap const& m_macro_map;
    bool m_detect_invalid_function_members;
};

class LibJSGCASTConsumer : public clang::ASTConsumer {
public:
    LibJSGCASTConsumer(clang::CompilerInstance&, bool detect_invalid_function_members);

private:
    virtual void HandleTranslationUnit(clang::ASTContext& context) override;

    clang::CompilerInstance& m_compiler;
    LibJSCellMacroMap m_macro_map;
    bool m_detect_invalid_function_members;
};

class LibJSGCPluginAction : public clang::PluginASTAction {
public:
    virtual bool ParseArgs(clang::CompilerInstance const&, std::vector<std::string> const& args) override
    {
        m_detect_invalid_function_members = std::find(args.begin(), args.end(), "detect-invalid-function-members") != args.end();
        return true;
    }

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler, llvm::StringRef) override
    {
        return std::make_unique<LibJSGCASTConsumer>(compiler, m_detect_invalid_function_members);
    }

    ActionType getActionType() override
    {
        return AddAfterMainAction;
    }

private:
    bool m_detect_invalid_function_members { false };
};
