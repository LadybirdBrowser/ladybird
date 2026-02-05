/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LambdaCapturePluginAction.h"
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Frontend/FrontendPluginRegistry.h>

AST_MATCHER_P(clang::Decl, hasAnnotation, std::string, name)
{
    (void)Builder;
    (void)Finder;
    for (auto const* attr : Node.attrs()) {
        if (auto const* annotate_attr = llvm::dyn_cast<clang::AnnotateAttr>(attr)) {
            if (annotate_attr->getAnnotation() == name)
                return true;
        }
    }
    return false;
}

// FIXME: Detect simple lambda escape patterns so we can enforce ESCAPING annotations in the most common cases
class Consumer
    : public clang::ASTConsumer
    , public clang::ast_matchers::internal::CollectMatchesCallback {
public:
    Consumer()
    {
        using namespace clang::ast_matchers;

        auto non_capturable_var_decl = varDecl(
            hasLocalStorage(),
            unless(
                anyOf(
                    // The declaration has an annotation:
                    //     IGNORE_USE_IN_ESCAPING_LAMBDA Foo foo;
                    hasAnnotation("serenity::ignore_use_in_escaping_lambda"),
                    // The declaration is a reference:
                    //     Foo& foo_ref = get_foo_ref();
                    //     Foo* foo_ptr = get_foo_ptr();
                    //     do_something([&foo_ref, &foo_ptr] {
                    //         foo_ref.foo();  // Fine, foo_ref references the underlying Foo instance
                    //         foo_ptr->foo(); // Bad, foo_ptr references the pointer on the stack above
                    //     });
                    hasType(references(TypeMatcher(anything()))))));

        auto bad_lambda_capture = lambdaCapture(anyOf(capturesThis(), capturesVar(non_capturable_var_decl))).bind("lambda-capture");

        auto lambda_with_bad_capture = lambdaExpr(
            anyOf(
                // These are both required as they have slightly different behavior.
                //
                // We need forEachLambdaCapture because we need to go over every explicit capture in the capture list, as
                // hasAnyCapture will just take the first capture in the list that matches the criteria (usually the `this`
                // capture). Without it, if the first capture in the list was flagged as bad but is actually fine (e.g. the
                // `this` capture, or a var capture by value), but there was a second capture in the list that was invalid,
                // it would be skipped.
                //
                // But forEachLambdaCapture doesn't seem to find implicit captures, so we also need hasAnyCapture to handle
                // captures that aren't explicitly listed in the capture list, but are still invalid.
                forEachLambdaCapture(bad_lambda_capture),
                hasAnyCapture(bad_lambda_capture)));

        // Bind this varDecl so we can reference it later to make sure it isn't being called
        auto lambda_with_bad_capture_decl = varDecl(hasInitializer(lambda_with_bad_capture)).bind("lambda");

        m_finder.addMatcher(
            traverse(
                clang::TK_IgnoreUnlessSpelledInSource,
                callExpr(
                    forEachArgumentWithParam(
                        anyOf(
                            // Match a lambda given directly in the function call
                            lambda_with_bad_capture,
                            // Matches an expression with a possibly-deeply-nested reference to a variable with a lambda type, e.g:
                            //     auto lambda = [...] { ... };
                            //     some_func(move(lambda));
                            has(declRefExpr(
                                to(lambda_with_bad_capture_decl),
                                // Avoid immediately invoked lambdas (i.e. match `move(lambda)` but not `move(lambda())`)
                                unless(hasParent(
                                    // <lambda struct>::operator()(...)
                                    cxxOperatorCallExpr(has(declRefExpr(to(equalsBoundNode("lambda")))))))))),
                        parmVarDecl(hasAnnotation("serenity::escaping")).bind("lambda-param-ref")))),
            this);
    }

    void HandleTranslationUnit(clang::ASTContext& Ctx) override
    {
        m_finder.matchAST(Ctx);
    }

    void run(clang::ast_matchers::MatchFinder::MatchResult const& result) override
    {
        auto& diag_engine = result.Context->getDiagnostics();

        if (auto const* capture = result.Nodes.getNodeAs<clang::LambdaCapture>("lambda-capture")) {
            if (capture->capturesThis() || capture->getCaptureKind() != clang::LCK_ByRef)
                return;

            auto diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Error, "Variable with local storage is captured by reference in a lambda marked ESCAPING");
            diag_engine.Report(capture->getLocation(), diag_id);

            clang::SourceLocation captured_var_location;
            if (auto const* var_decl = llvm::dyn_cast<clang::VarDecl>(capture->getCapturedVar())) {
                captured_var_location = var_decl->getTypeSourceInfo()->getTypeLoc().getBeginLoc();
            } else {
                captured_var_location = capture->getCapturedVar()->getLocation();
            }
            diag_id = diag_engine.getCustomDiagID(clang::DiagnosticsEngine::Note, "Annotate the variable declaration with IGNORE_USE_IN_ESCAPING_LAMBDA if it outlives the lambda");
            diag_engine.Report(captured_var_location, diag_id);
        }
    }

private:
    clang::ast_matchers::MatchFinder m_finder;
};

std::unique_ptr<clang::ASTConsumer> LambdaCapturePluginAction::CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef)
{
    return std::make_unique<Consumer>();
}

static clang::FrontendPluginRegistry::Add<LambdaCapturePluginAction> X("lambda_capture", "analyze lambda captures");
