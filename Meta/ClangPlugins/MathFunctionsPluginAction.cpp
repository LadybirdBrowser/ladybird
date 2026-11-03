/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MathFunctionsPluginAction.h"
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <llvm/ADT/StringSet.h>
#include <optional>

// The math functions that the C library of each platform computes differently.
static llvm::StringSet<> const math_functions = {
    "acos",
    "acosh",
    "asin",
    "asinh",
    "atan",
    "atan2",
    "atanh",
    "cbrt",
    "cos",
    "cosh",
    "exp",
    "exp2",
    "expm1",
    "hypot",
    "log",
    "log10",
    "log1p",
    "log2",
    "pow",
    "sin",
    "sincos",
    "sinh",
    "tan",
    "tanh",
};

static std::optional<llvm::StringRef> math_function_name(clang::FunctionDecl const& function, clang::SourceManager const& source_manager)
{
    if (!function.getDeclName().isIdentifier())
        return {};

    // A function of our own that shares a name with one of the C library is fine.
    if (!function.getBuiltinID() && !source_manager.isInSystemHeader(function.getCanonicalDecl()->getLocation()))
        return {};

    clang::NamespaceDecl const* outermost_namespace = nullptr;
    for (auto const* context = function.getDeclContext(); context; context = context->getParent()) {
        if (auto const* namespace_decl = llvm::dyn_cast<clang::NamespaceDecl>(context))
            outermost_namespace = namespace_decl;
    }
    if (outermost_namespace && outermost_namespace->getName() != "std")
        return {};

    auto name = function.getName();
    name.consume_front("__builtin_");
    if (math_functions.contains(name))
        return name;
    if ((name.ends_with("f") || name.ends_with("l")) && math_functions.contains(name.drop_back()))
        return name.drop_back();
    return {};
}

bool MathFunctionsVisitor::TraverseDecl(clang::Decl* declaration)
{
    if (declaration) {
        auto location = declaration->getLocation();
        if (location.isValid() && m_context.getSourceManager().isInSystemHeader(location))
            return true;
    }
    return RecursiveASTVisitor::TraverseDecl(declaration);
}

bool MathFunctionsVisitor::VisitDeclRefExpr(clang::DeclRefExpr* reference)
{
    auto const* function = llvm::dyn_cast<clang::FunctionDecl>(reference->getDecl());
    if (!function)
        return true;
    auto& source_manager = m_context.getSourceManager();
    auto name = math_function_name(*function, source_manager);
    if (!name)
        return true;

    auto location = source_manager.getFileLoc(reference->getLocation());
    if (source_manager.isInSystemHeader(location))
        return true;

    auto& diag_engine = m_context.getDiagnostics();
    auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "%0 is computed differently by the C library of each platform. Use AK::%1 from AK/Math.h instead");
    diag_engine.Report(reference->getBeginLoc(), diag_id) << function << *name;
    return true;
}

void MathFunctionsASTConsumer::HandleTranslationUnit(clang::ASTContext& context)
{
    MathFunctionsVisitor visitor(context);
    visitor.TraverseDecl(context.getTranslationUnitDecl());
}

static clang::FrontendPluginRegistry::Add<MathFunctionsPluginAction> X("math_functions", "check that math functions come from AK/Math.h");
