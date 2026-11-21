/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Derives of `libipc_rust::wire::Encode` and `libipc_rust::wire::Decode`.
//!
//! A struct goes as its fields in declaration order. An enum of unit variants with an integer `repr` goes as the
//! integer of its variant. Any other enum goes as the index of its variant, as a `u8`, then the variant's fields, as an
//! `AK::Variant` goes.
//!
//! Attributes:
//! - `#[ipc(skip = EXPR)]` on a field: the field is not sent, and decodes as `EXPR`.
//! - `#[ipc(max_size = N)]` on a field: the field decodes with `Decoder::decode_with_max_size(N)`.
//! - `#[ipc(validate = PATH)]` on a type: `PATH(&self) -> Result<(), &'static str>` refuses a value before it is
//!   encoded and after it is decoded.

use proc_macro::TokenStream;
use proc_macro2::{Literal, TokenStream as TokenStream2};
use quote::{format_ident, quote};
use syn::{
    Attribute, Data, DataEnum, DeriveInput, Expr, Fields, Generics, Ident, Index, Member, Path, Type, Variant,
    parse_macro_input, parse_quote,
};

#[proc_macro_derive(Encode, attributes(ipc))]
pub fn derive_encode(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand_encode(&input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

#[proc_macro_derive(Decode, attributes(ipc))]
pub fn derive_decode(input: TokenStream) -> TokenStream {
    let input = parse_macro_input!(input as DeriveInput);
    expand_decode(&input)
        .unwrap_or_else(syn::Error::into_compile_error)
        .into()
}

const INTEGER_REPRS: [&str; 8] = ["u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64"];

struct TypeAttributes {
    validate: Option<Path>,
}

fn type_attributes(attributes: &[Attribute]) -> syn::Result<TypeAttributes> {
    let mut validate = None;
    for attribute in attributes.iter().filter(|attribute| attribute.path().is_ident("ipc")) {
        attribute.parse_nested_meta(|meta| {
            if meta.path.is_ident("validate") {
                validate = Some(meta.value()?.parse()?);
                return Ok(());
            }
            Err(meta.error("expected `validate = PATH`"))
        })?;
    }
    Ok(TypeAttributes { validate })
}

enum FieldDecoding {
    Decoded,
    DecodedWithMaxSize(Expr),
    Skipped(Expr),
}

struct FieldInfo {
    member: Member,
    binding: Ident,
    ty: Type,
    decoding: FieldDecoding,
}

impl FieldInfo {
    fn is_sent(&self) -> bool {
        !matches!(self.decoding, FieldDecoding::Skipped(_))
    }

    fn decode_expression(&self) -> TokenStream2 {
        match &self.decoding {
            FieldDecoding::Decoded => quote!(decoder.decode()?),
            FieldDecoding::DecodedWithMaxSize(max_size) => quote!(decoder.decode_with_max_size(#max_size)?),
            FieldDecoding::Skipped(value) => quote!(#value),
        }
    }
}

fn field_infos(fields: &Fields) -> syn::Result<Vec<FieldInfo>> {
    fields
        .iter()
        .enumerate()
        .map(|(index, field)| {
            let member = match &field.ident {
                Some(ident) => Member::Named(ident.clone()),
                None => Member::Unnamed(Index::from(index)),
            };
            let mut decoding = FieldDecoding::Decoded;
            for attribute in field.attrs.iter().filter(|attribute| attribute.path().is_ident("ipc")) {
                attribute.parse_nested_meta(|meta| {
                    if !matches!(decoding, FieldDecoding::Decoded) {
                        return Err(meta.error("a field takes one of `skip` and `max_size`"));
                    }
                    if meta.path.is_ident("skip") {
                        decoding = FieldDecoding::Skipped(meta.value()?.parse()?);
                        return Ok(());
                    }
                    if meta.path.is_ident("max_size") {
                        decoding = FieldDecoding::DecodedWithMaxSize(meta.value()?.parse()?);
                        return Ok(());
                    }
                    Err(meta.error("expected `skip = EXPR` or `max_size = N`"))
                })?;
            }
            Ok(FieldInfo {
                member,
                binding: format_ident!("field_{}", index),
                ty: field.ty.clone(),
                decoding,
            })
        })
        .collect()
}

fn integer_repr(attributes: &[Attribute]) -> syn::Result<Option<Ident>> {
    let mut repr = None;
    for attribute in attributes.iter().filter(|attribute| attribute.path().is_ident("repr")) {
        attribute.parse_nested_meta(|meta| {
            if let Some(ident) = meta.path.get_ident()
                && INTEGER_REPRS.contains(&ident.to_string().as_str())
            {
                repr = Some(ident.clone());
            }
            if meta.input.peek(syn::token::Paren) {
                let _arguments;
                syn::parenthesized!(_arguments in meta.input);
            }
            Ok(())
        })?;
    }
    Ok(repr)
}

enum EnumForm {
    Integer(Ident),
    Tagged,
}

fn enum_form(input: &DeriveInput, data: &DataEnum) -> syn::Result<EnumForm> {
    if data.variants.is_empty() {
        return Err(syn::Error::new_spanned(
            &input.ident,
            "an enum without variants cannot be sent",
        ));
    }
    let is_unit_only = data.variants.iter().all(|variant| variant.fields.is_empty());
    if let Some(repr) = integer_repr(&input.attrs)?
        && is_unit_only
    {
        if input.generics.params.iter().next().is_some() {
            return Err(syn::Error::new_spanned(
                &input.generics,
                "an enum going as an integer cannot be generic",
            ));
        }
        return Ok(EnumForm::Integer(repr));
    }
    if let Some(variant) = data.variants.iter().find(|variant| variant.discriminant.is_some()) {
        return Err(syn::Error::new_spanned(
            variant,
            "an enum with discriminants goes as an integer, so it needs unit variants and an integer repr",
        ));
    }
    if data.variants.len() > 256 {
        return Err(syn::Error::new_spanned(&input.ident, "a variant index goes as a u8"));
    }
    Ok(EnumForm::Tagged)
}

fn generics_bound_by(generics: &Generics, bound: TokenStream2) -> Generics {
    let mut generics = generics.clone();
    for parameter in generics.type_params_mut() {
        parameter.bounds.push(parse_quote!(#bound));
    }
    generics
}

fn variant_tag(index: usize) -> Literal {
    Literal::u8_suffixed(u8::try_from(index).expect("the variant count is checked"))
}

fn struct_pattern(path: TokenStream2, fields: &[FieldInfo]) -> TokenStream2 {
    let sent: Vec<_> = fields.iter().filter(|field| field.is_sent()).collect();
    let members = sent.iter().map(|field| &field.member);
    let bindings = sent.iter().map(|field| &field.binding);
    quote!(#path { #(#members: #bindings,)* .. })
}

fn expand_encode(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let name = &input.ident;
    let generics = generics_bound_by(&input.generics, quote!(::libipc_rust::wire::Encode));
    let (impl_generics, type_generics, where_clause) = generics.split_for_impl();
    let attributes = type_attributes(&input.attrs)?;
    let (validate_borrowed, validate_owned) = match &attributes.validate {
        Some(validate) => (
            quote!(#validate(self).map_err(::libipc_rust::wire::EncodeError::InvalidValue)?;),
            quote!(#validate(&self).map_err(::libipc_rust::wire::EncodeError::InvalidValue)?;),
        ),
        None => (quote!(), quote!()),
    };

    let mut encode_uses_encoder = true;
    let (encode_body, encode_owned_body) = match &input.data {
        Data::Struct(data) => {
            let fields = field_infos(&data.fields)?;
            let sent: Vec<_> = fields.iter().filter(|field| field.is_sent()).collect();
            encode_uses_encoder = !sent.is_empty();
            let members = sent.iter().map(|field| &field.member);
            let encode = quote! {
                #validate_borrowed
                #(encoder.encode(&self.#members)?;)*
                ::core::result::Result::Ok(())
            };
            let encode_owned = (!sent.is_empty()).then(|| {
                let pattern = struct_pattern(quote!(Self), &fields);
                let bindings = sent.iter().map(|field| &field.binding);
                quote! {
                    #validate_owned
                    let #pattern = self;
                    #(encoder.encode_owned(#bindings)?;)*
                    ::core::result::Result::Ok(())
                }
            });
            (encode, encode_owned)
        }
        Data::Enum(data) => match enum_form(input, data)? {
            EnumForm::Integer(repr) => {
                let variants = data.variants.iter().map(|variant| &variant.ident);
                let encode = quote! {
                    #validate_borrowed
                    let value: #repr = match self {
                        #(Self::#variants => Self::#variants as #repr,)*
                    };
                    encoder.encode(&value)
                };
                (encode, None)
            }
            EnumForm::Tagged => {
                let variants = data
                    .variants
                    .iter()
                    .map(|variant| Ok((variant, field_infos(&variant.fields)?)))
                    .collect::<syn::Result<Vec<(&Variant, Vec<FieldInfo>)>>>()?;
                let arms = |method: TokenStream2| {
                    variants.iter().enumerate().map(move |(index, (variant, fields))| {
                        let ident = &variant.ident;
                        let pattern = struct_pattern(quote!(Self::#ident), fields);
                        let tag = variant_tag(index);
                        let bindings = fields
                            .iter()
                            .filter(|field| field.is_sent())
                            .map(|field| &field.binding);
                        quote! {
                            #pattern => {
                                encoder.encode(&#tag)?;
                                #(encoder.#method(#bindings)?;)*
                            }
                        }
                    })
                };
                let borrowed_arms = arms(quote!(encode));
                let encode = quote! {
                    #validate_borrowed
                    match self {
                        #(#borrowed_arms)*
                    }
                    ::core::result::Result::Ok(())
                };
                let has_sent_fields = variants.iter().any(|(_, fields)| fields.iter().any(FieldInfo::is_sent));
                let encode_owned = has_sent_fields.then(|| {
                    let owned_arms = arms(quote!(encode_owned));
                    quote! {
                        #validate_owned
                        match self {
                            #(#owned_arms)*
                        }
                        ::core::result::Result::Ok(())
                    }
                });
                (encode, encode_owned)
            }
        },
        Data::Union(_) => return Err(syn::Error::new_spanned(name, "a union cannot be sent")),
    };

    let encoder_parameter = if encode_uses_encoder {
        quote!(encoder)
    } else {
        quote!(_)
    };
    let encode_owned = encode_owned_body.map(|body| {
        quote! {
            #[inline]
            fn encode_owned(self, encoder: &mut ::libipc_rust::wire::Encoder) -> ::core::result::Result<(), ::libipc_rust::wire::EncodeError> {
                #body
            }
        }
    });

    Ok(quote! {
        #[automatically_derived]
        impl #impl_generics ::libipc_rust::wire::Encode for #name #type_generics #where_clause {
            #[inline]
            fn encode(&self, #encoder_parameter: &mut ::libipc_rust::wire::Encoder) -> ::core::result::Result<(), ::libipc_rust::wire::EncodeError> {
                #encode_body
            }

            #encode_owned
        }
    })
}

fn expand_decode(input: &DeriveInput) -> syn::Result<TokenStream2> {
    let name = &input.ident;
    let name_string = name.to_string();
    let generics = generics_bound_by(&input.generics, quote!(::libipc_rust::wire::Decode));
    let (impl_generics, type_generics, where_clause) = generics.split_for_impl();
    let attributes = type_attributes(&input.attrs)?;
    let invalid = quote!(::core::result::Result::Err(::libipc_rust::wire::DecodeError::InvalidValue(#name_string)));

    let mut decode_uses_decoder = true;
    let (min_encoded_size, value) = match &input.data {
        Data::Struct(data) => {
            let fields = field_infos(&data.fields)?;
            decode_uses_decoder = fields.iter().any(FieldInfo::is_sent);
            let sizes: Vec<_> = fields
                .iter()
                .filter(|field| field.is_sent())
                .map(|field| {
                    let ty = &field.ty;
                    quote!(<#ty as ::libipc_rust::wire::Decode>::MIN_ENCODED_SIZE)
                })
                .collect();
            let min_encoded_size = if sizes.is_empty() {
                quote!(0)
            } else {
                quote!(#(#sizes)+*)
            };
            let members = fields.iter().map(|field| &field.member);
            let values = fields.iter().map(FieldInfo::decode_expression);
            (min_encoded_size, quote!(Self { #(#members: #values,)* }))
        }
        Data::Enum(data) => match enum_form(input, data)? {
            EnumForm::Integer(repr) => {
                let variants: Vec<_> = data.variants.iter().map(|variant| &variant.ident).collect();
                let discriminants: Vec<_> = (0..variants.len())
                    .map(|index| format_ident!("DISCRIMINANT_{}", index))
                    .collect();
                let value = quote! {{
                    #(const #discriminants: #repr = #name::#variants as #repr;)*
                    match decoder.decode::<#repr>()? {
                        #(#discriminants => Self::#variants,)*
                        _ => return #invalid,
                    }
                }};
                (quote!(::core::mem::size_of::<#repr>()), value)
            }
            EnumForm::Tagged => {
                let arms = data
                    .variants
                    .iter()
                    .enumerate()
                    .map(|(index, variant)| {
                        let ident = &variant.ident;
                        let fields = field_infos(&variant.fields)?;
                        let members = fields.iter().map(|field| &field.member);
                        let values = fields.iter().map(FieldInfo::decode_expression);
                        let tag = Literal::usize_unsuffixed(index);
                        Ok(quote!(#tag => Self::#ident { #(#members: #values,)* },))
                    })
                    .collect::<syn::Result<Vec<_>>>()?;
                let value = quote! {
                    match decoder.decode::<u8>()? {
                        #(#arms)*
                        _ => return #invalid,
                    }
                };
                (quote!(::core::mem::size_of::<u8>()), value)
            }
        },
        Data::Union(_) => return Err(syn::Error::new_spanned(name, "a union cannot be sent")),
    };

    let decoder_parameter = if decode_uses_decoder {
        quote!(decoder)
    } else {
        quote!(_)
    };
    let body = match &attributes.validate {
        Some(validate) => quote! {
            let value = #value;
            #validate(&value).map_err(::libipc_rust::wire::DecodeError::InvalidValue)?;
            ::core::result::Result::Ok(value)
        },
        None => quote!(::core::result::Result::Ok(#value)),
    };

    Ok(quote! {
        #[automatically_derived]
        impl #impl_generics ::libipc_rust::wire::Decode for #name #type_generics #where_clause {
            const MIN_ENCODED_SIZE: usize = #min_encoded_size;

            #[inline]
            fn decode(#decoder_parameter: &mut ::libipc_rust::wire::Decoder<'_>) -> ::core::result::Result<Self, ::libipc_rust::wire::DecodeError> {
                #body
            }
        }
    })
}
