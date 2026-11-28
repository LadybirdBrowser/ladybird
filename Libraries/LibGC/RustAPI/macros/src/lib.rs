/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The derive of libgc_api's Trace.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::{format_ident, quote};
use syn::{Data, DeriveInput, Fields, Index, parse_macro_input};

/// Implements libgc_api's Trace by tracing every field, so that every field's type has to implement Trace: a field that
/// holds cells in a type that does not report them, like a raw pointer, fails to compile rather than go untraced.
///
/// A field marked `#[gc(untraced)]` is skipped. The form `#[gc(untraced = "...")]` also says why, where the field's
/// type does not tell: it holds no cells, or what keeps them alive, or that it holds them weakly.
/// A field marked `#[gc(owned = "why")]` holds its cells in memory that the cell owns outside the heap, and says why
/// nothing frees, moves or writes them until the collection is over, so that they are visited as owned slots (see
/// Visitor::visit_owned_slots()).
#[proc_macro_derive(Trace, attributes(gc))]
pub fn derive_trace(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let name = &input.ident;
    let (impl_generics, type_generics, where_clause) = input.generics.split_for_impl();
    let body = match &input.data {
        Data::Struct(data) => trace_fields(&data.fields, |index, field| match &field.ident {
            Some(ident) => quote!(&self.#ident),
            None => {
                let index = Index::from(index);
                quote!(&self.#index)
            }
        }),
        Data::Enum(data) => data
            .variants
            .iter()
            .map(|variant| {
                let variant_name = &variant.ident;
                let bindings: Vec<_> = (0..variant.fields.len())
                    .map(|index| format_ident!("field_{index}"))
                    .collect();
                let traced = trace_fields(&variant.fields, |index, _| {
                    let binding = &bindings[index];
                    quote!(#binding)
                })?;
                Ok(match &variant.fields {
                    Fields::Named(fields) => {
                        let names = fields.named.iter().map(|field| field.ident.as_ref().unwrap());
                        quote!(Self::#variant_name { #(#names: #bindings),* } => { #traced })
                    }
                    Fields::Unnamed(_) => quote!(Self::#variant_name(#(#bindings),*) => { #traced }),
                    Fields::Unit => quote!(Self::#variant_name => {}),
                })
            })
            .collect::<syn::Result<Vec<_>>>()
            .map(|arms| quote!(match self { #(#arms)* })),
        Data::Union(_) => {
            return syn::Error::new_spanned(name, "Trace cannot be derived for a union")
                .to_compile_error()
                .into();
        }
    };
    let body = match body {
        Ok(body) => body,
        Err(error) => return error.to_compile_error().into(),
    };
    if let Err(error) = check_type_attributes(&input) {
        return error.to_compile_error().into();
    }
    let lifetime = free_lifetime(&input.generics, "__c");
    quote! {
        // SAFETY: Traces every field that is not marked untraced.
        unsafe impl #impl_generics ::libgc_api::visitor::Trace for #name #type_generics #where_clause {
            #[allow(unused_variables)]
            #[inline]
            fn trace<#lifetime>(&#lifetime self, visitor: &mut ::libgc_api::visitor::Visitor<#lifetime>) {
                #body
            }
        }
    }
    .into()
}

/// The type itself takes no #[gc] attributes.
fn check_type_attributes(input: &DeriveInput) -> syn::Result<()> {
    match input.attrs.iter().find(|attribute| attribute.path().is_ident("gc")) {
        Some(attribute) => Err(syn::Error::new_spanned(attribute, "#[gc] only marks fields")),
        None => Ok(()),
    }
}

/// A lifetime named `base`, with as many more underscores in front as it takes for `generics` not to have it.
fn free_lifetime(generics: &syn::Generics, base: &str) -> syn::Lifetime {
    let mut name = base.to_string();
    while generics.lifetimes().any(|lifetime| lifetime.lifetime.ident == name) {
        name.insert(0, '_');
    }
    syn::Lifetime::new(&format!("'{name}"), proc_macro2::Span::call_site())
}

/// The trace of `fields`, each of which `access` gives a reference to by its index.
fn trace_fields(fields: &Fields, access: impl Fn(usize, &syn::Field) -> TokenStream2) -> syn::Result<TokenStream2> {
    let mut traced = Vec::new();
    for (index, field) in fields.iter().enumerate() {
        let value = access(index, field);
        traced.push(match field_tracing(field)? {
            FieldTracing::Untraced => continue,
            // SAFETY: The field's #[gc(owned = "...")] says why nothing frees, moves or writes its slots until the
            //         collection is over.
            FieldTracing::Owned => quote!(unsafe { visitor.visit_owned_slots(#value) };),
            FieldTracing::Traced => quote!(::libgc_api::visitor::Trace::trace(#value, visitor);),
        });
    }
    Ok(quote!(#(#traced)*))
}

/// How the trace visits a field, from its #[gc(...)] attributes.
enum FieldTracing {
    Traced,
    /// #[gc(untraced)] or #[gc(untraced = "what keeps the cells alive")]: skipped.
    Untraced,
    /// #[gc(owned = "why")]: its slots are in memory that the cell owns outside the heap, which nothing frees, moves or
    /// writes until the collection is over, so they are visited as owned slots (see Visitor::visit_owned_slots()).
    Owned,
}

fn field_tracing(field: &syn::Field) -> syn::Result<FieldTracing> {
    let mut tracing = FieldTracing::Traced;
    for attribute in field.attrs.iter().filter(|attribute| attribute.path().is_ident("gc")) {
        attribute.parse_nested_meta(|meta| {
            if !matches!(tracing, FieldTracing::Traced) {
                return Err(meta.error("a field is either untraced or owned, once"));
            }
            if meta.path.is_ident("untraced") {
                if meta.input.peek(syn::Token![=]) {
                    meta.value()?.parse::<syn::LitStr>()?;
                }
                tracing = FieldTracing::Untraced;
                return Ok(());
            }
            if meta.path.is_ident("owned") {
                meta.value()?.parse::<syn::LitStr>()?;
                tracing = FieldTracing::Owned;
                return Ok(());
            }
            Err(meta.error("expected `untraced`, `untraced = \"why\"` or `owned = \"why\"`"))
        })?;
    }
    Ok(tracing)
}
