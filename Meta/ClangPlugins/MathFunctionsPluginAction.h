/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/FrontendAction.h>

class MathFunctionsVisitor : public clang::RecursiveASTVisitor<MathFunctionsVisitor> {
public:
    explicit MathFunctionsVisitor(clang::ASTContext& context)
        : m_context(context)
    {
    }

    bool shouldVisitTemplateInstantiations() const { return true; }

    bool TraverseDecl(clang::Decl*);
    bool VisitDeclRefExpr(clang::DeclRefExpr*);

private:
    clang::ASTContext& m_context;
};

class MathFunctionsASTConsumer : public clang::ASTConsumer {
private:
    virtual void HandleTranslationUnit(clang::ASTContext&) override;
};

class MathFunctionsPluginAction : public clang::PluginASTAction {
public:
    virtual bool ParseArgs(clang::CompilerInstance const&, std::vector<std::string> const&) override
    {
        return true;
    }

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override
    {
        return std::make_unique<MathFunctionsASTConsumer>();
    }

    ActionType getActionType() override
    {
        return AddAfterMainAction;
    }
};
