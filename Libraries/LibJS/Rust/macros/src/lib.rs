/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Derives for the Rust LibJS runtime.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::{format_ident, quote};
use syn::{Data, DeriveInput, Fields, Index, ItemFn, parse_macro_input, parse_quote};

/// The name of what `jit_abi` declares next to a function, which `jit_function_address!` checks for.
fn jit_abi_marker(function: &syn::Ident) -> syn::Ident {
    format_ident!("{}_has_jit_abi", function)
}

/// Marks an `extern "C"` function that JIT code calls, and gives it the calling convention JIT code calls with. JIT code
/// uses the System V calling convention on every x86-64 target, so on Windows, the function has that instead of the
/// platform's. Taking the address of a function erases its calling convention, so JIT code gets the address from
/// `jit_function_address!`, which only builds for functions with this attribute.
///
/// NB: This expands to both functions behind `cfg` attributes, since `cfg!` here would see the host and not the target.
#[proc_macro_attribute]
pub fn jit_abi(attributes: TokenStream, item: TokenStream) -> TokenStream {
    if !attributes.is_empty() {
        return syn::Error::new_spanned(TokenStream2::from(attributes), "jit_abi takes no arguments")
            .to_compile_error()
            .into();
    }
    let function = parse_macro_input!(item as ItemFn);
    let is_extern_c = function
        .sig
        .abi
        .as_ref()
        .and_then(|abi| abi.name.as_ref())
        .is_some_and(|name| name.value() == "C");
    if !is_extern_c {
        return syn::Error::new_spanned(&function.sig, "jit_abi only goes on extern \"C\" functions")
            .to_compile_error()
            .into();
    }
    let mut system_v_function = function.clone();
    system_v_function.sig.abi = Some(parse_quote!(extern "sysv64"));
    let visibility = &function.vis;
    let marker = jit_abi_marker(&function.sig.ident);
    quote! {
        #[cfg(all(target_arch = "x86_64", windows))]
        #system_v_function
        #[cfg(not(all(target_arch = "x86_64", windows)))]
        #function
        #[doc(hidden)]
        #[allow(dead_code, non_upper_case_globals)]
        #visibility const #marker: () = ();
    }
    .into()
}

/// The address of a function that JIT code calls, as a `u64`, which only builds if the function has the `jit_abi`
/// attribute: `jit_function_address!(path::to::function)`. The path is the one the function is declared at, since the
/// check looks next to it.
#[proc_macro]
pub fn jit_function_address(input: TokenStream) -> TokenStream {
    let function = parse_macro_input!(input as syn::Path);
    let Some(last) = function.segments.last() else {
        return syn::Error::new_spanned(&function, "jit_function_address! takes the path of a function")
            .to_compile_error()
            .into();
    };
    let mut marker = function.clone();
    marker.segments.last_mut().expect("the path has a segment").ident = jit_abi_marker(&last.ident);
    quote! {
        {
            const _: () = #marker;
            #function as *const () as u64
        }
    }
    .into()
}

/// Implements Trace by tracing every field. A field marked #[gc(untraced)] is skipped and holds no cells, or says what
/// keeps them alive: #[gc(untraced = "the realm traces the shapes")]. A field marked #[gc(owned = "why")] holds its
/// cells in memory that the cell owns outside the heap, and says why nothing frees, moves or writes them until the
/// collection is over, so that they are visited as owned slots (see Visitor::visit_owned_slots()).
///
/// The Trace trait is crate::gc::visitor::Trace, or path::gc::visitor::Trace for a type marked #[gc(crate = path)],
/// such as a type that an embedder puts in cells of the runtime's heap from a crate of its own.
#[proc_macro_derive(Trace, attributes(gc))]
pub fn derive_trace(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    let name = &input.ident;
    let (impl_generics, type_generics, where_clause) = input.generics.split_for_impl();
    let gc = match gc_module_path(&input) {
        Ok(path) => path,
        Err(error) => return error.to_compile_error().into(),
    };
    let body = match &input.data {
        Data::Struct(data) => trace_fields(&data.fields, &gc, |index, field| match &field.ident {
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
                let traced = trace_fields(&variant.fields, &gc, |index, _| {
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
    let lifetime = free_lifetime(&input.generics, "__c");
    quote! {
        // SAFETY: Traces every field that is not marked untraced.
        unsafe impl #impl_generics #gc::visitor::Trace for #name #type_generics #where_clause {
            #[allow(unused_variables)]
            fn trace<#lifetime>(&#lifetime self, visitor: &mut #gc::visitor::Visitor<#lifetime>) {
                #body
            }
        }
    }
    .into()
}

/// A lifetime named `base`, with as many more underscores in front as it takes for `generics` not to have it.
fn free_lifetime(generics: &syn::Generics, base: &str) -> syn::Lifetime {
    let mut name = base.to_string();
    while generics.lifetimes().any(|lifetime| lifetime.lifetime.ident == name) {
        name.insert(0, '_');
    }
    syn::Lifetime::new(&format!("'{name}"), proc_macro2::Span::call_site())
}

/// The path of the module that has the visitor module, from the type's #[gc(crate = path)], or crate::gc.
fn gc_module_path(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let mut crate_path: Option<syn::Path> = None;
    for attribute in input.attrs.iter().filter(|attribute| attribute.path().is_ident("gc")) {
        attribute.parse_nested_meta(|meta| {
            if meta.path.is_ident("crate") {
                crate_path = Some(meta.value()?.parse()?);
                return Ok(());
            }
            Err(meta.error("expected `crate = path`"))
        })?;
    }
    Ok(match crate_path {
        Some(path) => quote!(#path::gc),
        None => quote!(crate::gc),
    })
}

/// The trace of `fields`, each of which `access` gives a reference to by its index.
fn trace_fields(
    fields: &Fields,
    gc: &TokenStream2,
    access: impl Fn(usize, &syn::Field) -> TokenStream2,
) -> syn::Result<TokenStream2> {
    let mut traced = Vec::new();
    for (index, field) in fields.iter().enumerate() {
        let value = access(index, field);
        traced.push(match field_tracing(field)? {
            FieldTracing::Untraced => continue,
            // SAFETY: The field's #[gc(owned = "...")] says why nothing frees, moves or writes its slots until the
            //         collection is over.
            FieldTracing::Owned => quote!(unsafe { visitor.visit_owned_slots(#value) };),
            FieldTracing::Traced => quote!(#gc::visitor::Trace::trace(#value, visitor);),
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
