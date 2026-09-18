/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/FrontendAction.h>
#include <set>
#include <vector>

class KmallocAllocationVisitor : public clang::RecursiveASTVisitor<KmallocAllocationVisitor> {
public:
    explicit KmallocAllocationVisitor(clang::ASTContext& context)
        : m_context(context)
    {
    }

    // Instantiations are traversed only for templates whose definition contains an allocation, a raw adoption or
    // a redeclared allocation operator, which keeps the traversal proportional to the code that can misallocate.
    bool shouldVisitTemplateInstantiations() const { return m_traversing_chosen_instantiations; }

    void collect_allocator_opt_outs();

    bool TraverseDecl(clang::Decl*);
    bool TraverseClassTemplateDecl(clang::ClassTemplateDecl*);
    bool TraverseFunctionTemplateDecl(clang::FunctionTemplateDecl*);
    bool TraverseRequiresExpr(clang::RequiresExpr*);
    bool TraverseCXXNoexceptExpr(clang::CXXNoexceptExpr*);
    bool TraverseUnaryExprOrTypeTraitExpr(clang::UnaryExprOrTypeTraitExpr*);
    bool TraverseTypeTraitExpr(clang::TypeTraitExpr*);
    bool TraverseDecltypeTypeLoc(clang::DecltypeTypeLoc);
    bool VisitCXXNewExpr(clang::CXXNewExpr*);
    bool VisitCXXConstructExpr(clang::CXXConstructExpr*);
    bool VisitCXXUnresolvedConstructExpr(clang::CXXUnresolvedConstructExpr*);
    bool VisitCallExpr(clang::CallExpr*);
    bool VisitDependentScopeDeclRefExpr(clang::DependentScopeDeclRefExpr*);
    bool VisitCXXRecordDecl(clang::CXXRecordDecl*);

private:
    bool is_project_record(clang::CXXRecordDecl const&) const;
    bool record_has_kmalloc_tag(clang::CXXRecordDecl const&) const;
    bool record_declares_kmalloc_tag(clang::CXXRecordDecl const&) const;
    bool is_opted_out(clang::QualType) const;
    bool use_site_is_inside_ak() const;
    void check_opt_out(clang::VarTemplateSpecializationDecl const&);

    template<typename Node, typename Traverse>
    bool traverse_unevaluated(Node node, Traverse traverse)
    {
        ++m_unevaluated_depth;
        bool result = traverse(node);
        --m_unevaluated_depth;
        return result;
    }

    template<typename Template, typename Traverse>
    bool traverse_template(Template* declaration, Traverse traverse)
    {
        bool was_traversing_chosen_instantiations = m_traversing_chosen_instantiations;
        m_traversing_chosen_instantiations = false;
        m_template_candidate_stack.push_back(false);
        bool result = traverse(declaration);
        bool is_candidate = m_template_candidate_stack.back();
        m_template_candidate_stack.pop_back();
        // A member template inside a class template instantiation carries no body of its own, so it inherits the
        // verdict of the member template it was instantiated from.
        bool inherits_candidacy = false;
        for (auto const* original = declaration->getInstantiatedFromMemberTemplate(); original; original = original->getInstantiatedFromMemberTemplate()) {
            if (m_candidate_templates.contains(original->getCanonicalDecl())) {
                inherits_candidacy = true;
                break;
            }
        }
        if (is_candidate)
            m_candidate_templates.insert(declaration->getCanonicalDecl());
        if (!m_template_candidate_stack.empty() && (is_candidate || inherits_candidacy))
            m_template_candidate_stack.back() = true;
        bool traverse_instantiations = (is_candidate && declaration->getTemplatedDecl()->isThisDeclarationADefinition()) || inherits_candidacy;
        if (result && traverse_instantiations) {
            m_traversing_chosen_instantiations = true;
            result = TraverseTemplateInstantiations(declaration);
        }
        m_traversing_chosen_instantiations = was_traversing_chosen_instantiations;
        return result;
    }

    void mark_template_candidate()
    {
        if (!m_template_candidate_stack.empty())
            m_template_candidate_stack.back() = true;
    }

    clang::ASTContext& m_context;
    std::set<clang::Type const*> m_opted_out_types;
    std::vector<clang::FunctionDecl const*> m_function_stack;
    std::vector<bool> m_template_candidate_stack;
    std::set<clang::TemplateDecl const*> m_candidate_templates;
    unsigned m_unevaluated_depth { 0 };
    bool m_traversing_chosen_instantiations { false };
};

class KmallocAllocationASTConsumer : public clang::ASTConsumer {
private:
    virtual void HandleTranslationUnit(clang::ASTContext&) override;
};

class KmallocAllocationPluginAction : public clang::PluginASTAction {
public:
    virtual bool ParseArgs(clang::CompilerInstance const&, std::vector<std::string> const&) override
    {
        return true;
    }

    virtual std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance&, llvm::StringRef) override
    {
        return std::make_unique<KmallocAllocationASTConsumer>();
    }

    ActionType getActionType() override
    {
        return AddAfterMainAction;
    }
};
