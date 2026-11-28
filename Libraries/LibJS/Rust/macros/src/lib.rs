/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Attributes for the Rust LibJS runtime.

use proc_macro::TokenStream;
use proc_macro2::TokenStream as TokenStream2;
use quote::{format_ident, quote};
use syn::{ItemFn, parse_macro_input, parse_quote};

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
