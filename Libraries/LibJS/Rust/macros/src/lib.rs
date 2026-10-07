/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Derives for the Rust LibJS runtime.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::{format_ident, quote};
use syn::{Data, DeriveInput, Fields, Index, parse_macro_input};

/// Implements Trace by tracing every field. A field marked #[gc(untraced)] is skipped; it has to hold no cells, or
/// cells something else keeps alive and the cell's owner clears once they die, like an inline cache's shapes.
#[proc_macro_derive(Trace, attributes(gc))]
pub fn derive_trace(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let name = &input.ident;
    let (impl_generics, type_generics, where_clause) = input.generics.split_for_impl();
    let body = match &input.data {
        Data::Struct(data) => trace_fields(&data.fields, |member| quote!(&self.#member)),
        Data::Enum(data) => {
            let arms = data.variants.iter().map(|variant| {
                let variant_name = &variant.ident;
                let bindings: Vec<_> = (0..variant.fields.len())
                    .map(|index| format_ident!("field_{index}"))
                    .collect();
                let traced: Vec<_> = variant
                    .fields
                    .iter()
                    .zip(&bindings)
                    .filter(|(field, _)| !is_untraced(field))
                    .map(|(_, binding)| quote!(crate::gc::visitor::Trace::trace(#binding, visitor);))
                    .collect();
                match &variant.fields {
                    Fields::Named(fields) => {
                        let names = fields.named.iter().map(|field| field.ident.as_ref().unwrap());
                        quote!(Self::#variant_name { #(#names: #bindings),* } => { #(#traced)* })
                    }
                    Fields::Unnamed(_) => quote!(Self::#variant_name(#(#bindings),*) => { #(#traced)* }),
                    Fields::Unit => quote!(Self::#variant_name => {}),
                }
            });
            quote!(match self { #(#arms)* })
        }
        Data::Union(_) => {
            return syn::Error::new_spanned(name, "Trace cannot be derived for a union")
                .to_compile_error()
                .into();
        }
    };
    quote! {
        // SAFETY: Traces every field that is not marked untraced.
        unsafe impl #impl_generics crate::gc::visitor::Trace for #name #type_generics #where_clause {
            #[allow(unused_variables)]
            fn trace(&self, visitor: &mut crate::gc::visitor::Visitor) {
                #body
            }
        }
    }
    .into()
}

fn trace_fields(fields: &Fields, access: impl Fn(TokenStream2) -> TokenStream2) -> TokenStream2 {
    let traced = fields
        .iter()
        .enumerate()
        .filter(|(_, field)| !is_untraced(field))
        .map(|(index, field)| {
            let member = match &field.ident {
                Some(ident) => quote!(#ident),
                None => {
                    let index = Index::from(index);
                    quote!(#index)
                }
            };
            let value = access(member);
            quote!(crate::gc::visitor::Trace::trace(#value, visitor);)
        });
    quote!(#(#traced)*)
}

fn is_untraced(field: &syn::Field) -> bool {
    field.attrs.iter().any(|attribute| {
        attribute.path().is_ident("gc")
            && attribute
                .parse_args::<syn::Ident>()
                .is_ok_and(|argument| argument == "untraced")
    })
}
