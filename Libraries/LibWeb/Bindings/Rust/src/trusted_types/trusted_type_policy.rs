/*
 * Copyright (c) 2025, Miguel Sacristán Izcue <miguel_tete17@hotmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TrustedTypePolicy, https://w3c.github.io/trusted-types/dist/spec/#trusted-type-policy

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{
    Gc, MarkedVec, Object, PrimitiveString, PropertyKey, Realm, Trace, Utf16View, Value, Vm, utf16_formatted,
};

use crate::bindings::callback::CallbackFunction;
#[cfg(web_idl_bindings)]
use crate::bindings::conversions::FromJs;
use crate::bindings::conversions::{to_dom_string, to_usv_string};
use crate::bindings::interface::{Ref, Wrappable, cell_from_abi, cell_into_abi, create_dictionary_object, wrap};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::sanitizer::SetHTMLUnsafeOptions;
use crate::html::sanitizer_configuration::{
    SanitizerConfiguration, SanitizerInput, SanitizerInputKind, get_a_sanitizer_from_sanitizer_input, input_from_abi,
};
use crate::html::window_or_worker_global_scope::trusted_types;
use crate::trusted_types::require_trusted_types_for_directive::{
    does_sink_require_trusted_types, should_sink_type_mismatch_violation_be_blocked_by_content_security_policy,
};
use crate::trusted_types::trusted_html::TrustedHTML;
use crate::trusted_types::trusted_html_parser_options::TrustedHTMLParserOptions;
use crate::trusted_types::trusted_script::TrustedScript;
use crate::trusted_types::trusted_script_url::TrustedScriptURL;
use crate::trusted_types::trusted_type_policy_factory::{
    HTML_NAMESPACE, SVG_NAMESPACE, get_trusted_type_data_for_attribute,
};

/// https://w3c.github.io/trusted-types/dist/spec/#trusted-type-policy-options
pub(crate) struct TrustedTypePolicyOptions {
    pub(crate) create_html: Option<CallbackFunction>,
    pub(crate) create_parser_options: Option<CallbackFunction>,
    pub(crate) create_script: Option<CallbackFunction>,
    pub(crate) create_script_url: Option<CallbackFunction>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum TrustedTypeName {
    TrustedHTML,
    TrustedScript,
    TrustedScriptURL,
}

impl TrustedTypeName {
    pub(crate) fn from_abi(value: u8) -> Self {
        match value {
            0 => Self::TrustedHTML,
            1 => Self::TrustedScript,
            2 => Self::TrustedScriptURL,
            _ => unreachable!("{value} is not a TrustedTypeName"),
        }
    }

    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::TrustedHTML => "TrustedHTML",
            Self::TrustedScript => "TrustedScript",
            Self::TrustedScriptURL => "TrustedScriptURL",
        }
    }
}

/// https://w3c.github.io/trusted-types/dist/spec/#typedefdef-trustedtype
pub(crate) enum TrustedType {
    TrustedHTML(Ref<TrustedHTML>),
    TrustedScript(Ref<TrustedScript>),
    TrustedScriptURL(Ref<TrustedScriptURL>),
}

impl TrustedType {
    pub(crate) fn data(&self) -> &Utf16String {
        match self {
            Self::TrustedHTML(value) => value.data(),
            Self::TrustedScript(value) => value.data(),
            Self::TrustedScriptURL(value) => value.data(),
        }
    }

    fn type_name(&self) -> TrustedTypeName {
        match self {
            Self::TrustedHTML(_) => TrustedTypeName::TrustedHTML,
            Self::TrustedScript(_) => TrustedTypeName::TrustedScript,
            Self::TrustedScriptURL(_) => TrustedTypeName::TrustedScriptURL,
        }
    }

    fn create(vm: &Vm, trusted_type_name: TrustedTypeName, data_string: Utf16String) -> Self {
        match trusted_type_name {
            TrustedTypeName::TrustedHTML => Self::TrustedHTML(TrustedHTML::create(vm, data_string)),
            TrustedTypeName::TrustedScript => Self::TrustedScript(TrustedScript::create(vm, data_string)),
            TrustedTypeName::TrustedScriptURL => Self::TrustedScriptURL(TrustedScriptURL::create(vm, data_string)),
        }
    }
}

pub(crate) enum TrustedTypeOrString {
    TrustedType(TrustedType),
    String(Utf16String),
}

impl TrustedTypeOrString {
    pub(crate) fn stringified(&self) -> Utf16String {
        match self {
            Self::TrustedType(value) => value.data().clone(),
            Self::String(value) => value.clone(),
        }
    }
}

fn stringify(vm: &Vm, trusted_type_name: TrustedTypeName, policy_value: Value) -> WebIdlResult<Utf16String> {
    match trusted_type_name {
        TrustedTypeName::TrustedHTML | TrustedTypeName::TrustedScript => Ok(to_dom_string(vm, policy_value)?),
        TrustedTypeName::TrustedScriptURL => Ok(to_usv_string(vm, policy_value)?.into()),
    }
}

/// https://w3c.github.io/trusted-types/dist/spec/#trusted-type-policy
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TrustedTypePolicy {
    realm: Gc<Realm>,
    name: Utf16String,
    create_html: Option<CallbackFunction>,
    create_script: Option<CallbackFunction>,
    create_script_url: Option<CallbackFunction>,
    create_parser_options: Option<CallbackFunction>,
}

impl Wrappable for TrustedTypePolicy {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl TrustedTypePolicy {
    pub(crate) fn new(realm: Gc<Realm>, name: Utf16String, options: TrustedTypePolicyOptions) -> Self {
        Self {
            realm,
            name,
            create_html: options.create_html,
            create_script: options.create_script,
            create_script_url: options.create_script_url,
            create_parser_options: options.create_parser_options,
        }
    }

    pub(crate) fn create_parser_options_callback(&self) -> Option<&CallbackFunction> {
        self.create_parser_options.as_ref()
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.name.clone())
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createhtml
    pub(crate) fn create_html(
        &self,
        cx: &mut CallContext,
        input: Utf16String,
        arguments: MarkedVec<'_, Value>,
    ) -> WebIdlResult<Ref<TrustedHTML>> {
        // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
        //    policy
        //      this value
        //    trustedTypeName
        //      "TrustedHTML"
        //    value
        //      input
        //    arguments
        //      arguments
        match self.create_a_trusted_type(cx.vm(), TrustedTypeName::TrustedHTML, &input, &arguments)? {
            TrustedType::TrustedHTML(trusted_html) => Ok(trusted_html),
            _ => unreachable!("create a trusted type creates an object of the type name"),
        }
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createscript
    pub(crate) fn create_script(
        &self,
        cx: &mut CallContext,
        input: Utf16String,
        arguments: MarkedVec<'_, Value>,
    ) -> WebIdlResult<Ref<TrustedScript>> {
        // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
        //    policy
        //      this value
        //    trustedTypeName
        //      "TrustedScript"
        //    value
        //      input
        //    arguments
        //      arguments
        match self.create_a_trusted_type(cx.vm(), TrustedTypeName::TrustedScript, &input, &arguments)? {
            TrustedType::TrustedScript(trusted_script) => Ok(trusted_script),
            _ => unreachable!("create a trusted type creates an object of the type name"),
        }
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createscripturl
    pub(crate) fn create_script_url(
        &self,
        cx: &mut CallContext,
        input: Utf16String,
        arguments: MarkedVec<'_, Value>,
    ) -> WebIdlResult<Ref<TrustedScriptURL>> {
        // 1. Returns the result of executing the Create a Trusted Type algorithm, with the following arguments:
        //    policy
        //      this value
        //    trustedTypeName
        //      "TrustedScriptURL"
        //    value
        //      input
        //    arguments
        //      arguments
        match self.create_a_trusted_type(cx.vm(), TrustedTypeName::TrustedScriptURL, &input, &arguments)? {
            TrustedType::TrustedScriptURL(trusted_script_url) => Ok(trusted_script_url),
            _ => unreachable!("create a trusted type creates an object of the type name"),
        }
    }

    // https://w3c.github.io/trusted-types/dist/spec/#dom-trustedtypepolicy-createparseroptions
    pub(crate) fn create_parser_options(
        &self,
        cx: &mut CallContext,
        options: SetHTMLUnsafeOptions,
        arguments: MarkedVec<'_, Value>,
    ) -> WebIdlResult<Ref<TrustedHTMLParserOptions>> {
        let vm = cx.vm();

        // Returns the result of executing the create a trusted type algorithm, with the following arguments:
        //   policy: this value
        //   trustedTypeName: "TrustedHTMLParserOptions"
        //   value: options
        //   arguments: arguments
        // https://w3c.github.io/trusted-types/dist/spec/#create-a-trusted-type-algorithm
        // 1. Let policyValue be the result of executing get trusted type policy value with the same arguments as this
        //    algorithm and additionally true as throwIfMissing.
        // 2. If the algorithm threw an error, rethrow the error and abort the following steps.
        let policy_value = to_optional_set_html_unsafe_options(
            vm,
            get_parser_options_policy_value(
                vm,
                self.realm,
                self.create_parser_options.as_ref(),
                &options,
                &arguments.values(),
                true,
            )?,
        )?;

        // 3. If trustedTypeName is "TrustedHTMLParserOptions":
        // 1. If policyValue is null, throw a TypeError.
        // NOTE: Returning null from a named policy's createParserOptions callback throws a TypeError here, whereas
        //       returning null or undefined from createHTML, createScript, or createScriptURL yields the empty string.
        let Some(policy_value) = policy_value else {
            return Err(Exception::type_error("createParserOptions returned null"));
        };

        // 2. Let sanitizerConfig be the result of getting a sanitizer configuration from policy's relevant realm and
        //    policyValue.
        let sanitizer_config = get_a_sanitizer_config_copy_from_dictionary(vm, &policy_value)?;

        // 3. Let runScripts be policyValue["runScripts"] with default false.
        let run_scripts = policy_value.run_scripts;

        // 4. Return a new instance of TrustedHTMLParserOptions in policy's relevant realm with its sanitizer configuration
        //    set to sanitizerConfig, and its runScripts set to runScripts.
        Ok(TrustedHTMLParserOptions::create(
            vm,
            self.realm,
            sanitizer_config,
            run_scripts,
        ))
    }

    // https://w3c.github.io/trusted-types/dist/spec/#create-a-trusted-type-algorithm
    fn create_a_trusted_type(
        &self,
        vm: &Vm,
        trusted_type_name: TrustedTypeName,
        value: &Utf16String,
        arguments: &MarkedVec<'_, Value>,
    ) -> WebIdlResult<TrustedType> {
        // 1. Let policyValue be the result of executing Get Trusted Type policy value with the same arguments
        // as this algorithm and additionally true as throwIfMissing.
        // 2. If the algorithm threw an error, rethrow the error and abort the following steps.
        let policy_value =
            self.get_trusted_type_policy_value(vm, trusted_type_name, value, &arguments.values(), true)?;

        // 3. Let dataString be the result of stringifying policyValue.
        let mut data_string = stringify(vm, trusted_type_name, policy_value)?;

        // 4. If policyValue is null or undefined, set dataString to the empty string.
        if policy_value.is_nullish() {
            data_string = Utf16String::default();
        }

        // 5. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
        Ok(TrustedType::create(vm, trusted_type_name, data_string))
    }

    // https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-policy-value
    fn get_trusted_type_policy_value(
        &self,
        vm: &Vm,
        trusted_type_name: TrustedTypeName,
        value: &Utf16String,
        values: &[Value],
        throw_if_missing: bool,
    ) -> WebIdlResult<Value> {
        // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
        // 2. Let function be policy’s options[functionName].
        let function = match trusted_type_name {
            TrustedTypeName::TrustedHTML => &self.create_html,
            TrustedTypeName::TrustedScript => &self.create_script,
            TrustedTypeName::TrustedScriptURL => &self.create_script_url,
        };

        // 3. If function is null, then:
        let Some(function) = function else {
            // 1. If throwIfMissing throw a TypeError.
            if throw_if_missing {
                return Err(Exception::type_error(
                    "Trying to create a trusted type without a callback",
                ));
            }

            // 2. Else return null
            return Ok(Value::NULL);
        };

        // 4. Let args be << value >>.
        let args = MarkedVec::new(vm);
        args.push(Value::from_string(PrimitiveString::create(vm, value.clone())));

        // 5. Append each item in arguments to args.
        args.extend_from_slice(values);

        // 6. Let policyValue be the result of invoking function with args and "rethrow".
        let policy_value = function.invoke(None, &args.values())?;

        // 7. Return policyValue.
        Ok(policy_value)
    }
}

// https://w3c.github.io/trusted-types/dist/spec/#create-a-parser-options-callback-argument
fn create_a_parser_options_callback_argument(
    vm: &Vm,
    realm: Gc<Realm>,
    dictionary: &SetHTMLUnsafeOptions,
) -> WebIdlResult<Value> {
    // 1. Let runScriptsInput be dictionary["runScripts"], with default false.
    let run_scripts_input = dictionary.run_scripts;

    // 2. Let argument be the ordered map «[ "runScripts" → runScriptsInput ]».
    let cx = CallContext::new(vm, realm);
    let argument = create_dictionary_object(&cx);
    argument.create_data_property(
        vm,
        &PropertyKey::from_utf8("runScripts"),
        Value::from_bool(run_scripts_input),
        None,
        None,
    )?;

    // 3. Let sanitizerInput be the result of getting a sanitizer from realm and dictionary.
    // https://w3c.github.io/trusted-types/dist/spec/#get-a-sanitizer-from-dictionary
    // 1. Let sanitizerInput be dictionary["sanitizer"], defaulting to null.
    // 2. If sanitizerInput is null, return null.
    // 4. If sanitizerInput is not null, set argument["sanitizer"] to sanitizerInput.
    if let Some(sanitizer_input) = &dictionary.sanitizer {
        let sanitizer = get_a_sanitizer_from_sanitizer_input(vm, sanitizer_input)?;
        argument.create_data_property(
            vm,
            &PropertyKey::from_utf8("sanitizer"),
            Value::from_object(wrap(&cx, sanitizer)),
            None,
            None,
        )?;
    }

    // 5. Return argument.
    Ok(Value::from_object(argument))
}

// NB: Converts the return value of a CreateParserOptionsCallback, whose return type is SetHTMLUnsafeOptions?, to an
//     IDL value.
fn to_optional_set_html_unsafe_options(vm: &Vm, value: Value) -> WebIdlResult<Option<SetHTMLUnsafeOptions>> {
    if value.is_nullish() {
        return Ok(None);
    }
    #[cfg(web_idl_bindings)]
    return Ok(Some(SetHTMLUnsafeOptions::from_js(vm, value)?));
    #[cfg(not(web_idl_bindings))]
    {
        let _ = vm;
        unreachable!("converting to SetHTMLUnsafeOptions requires the generated bindings")
    }
}

// https://w3c.github.io/trusted-types/dist/spec/#get-a-sanitizer-config-copy-from-dictionary
fn get_a_sanitizer_config_copy_from_dictionary(
    vm: &Vm,
    dictionary: &SetHTMLUnsafeOptions,
) -> WebIdlResult<Option<SanitizerConfiguration>> {
    // 1. Let sanitizer be the result of getting a sanitizer from realm and dictionary.
    // 2. If sanitizer is null, return null.
    let Some(sanitizer_input) = &dictionary.sanitizer else {
        return Ok(None);
    };
    let sanitizer = get_a_sanitizer_from_sanitizer_input(vm, sanitizer_input)?;

    // 3. Return sanitizer's configuration.
    Ok(Some(sanitizer.configuration()))
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-policy-value
// NB: The steps for the "TrustedHTMLParserOptions" type name.
fn get_parser_options_policy_value(
    vm: &Vm,
    realm: Gc<Realm>,
    function: Option<&CallbackFunction>,
    value: &SetHTMLUnsafeOptions,
    arguments: &[Value],
    throw_if_missing: bool,
) -> WebIdlResult<Value> {
    // 1. Let functionName be a function name for the given trustedTypeName, based on the following table:
    // 2. Let function be policy's options[functionName].
    // NB: The caller passes function.

    // 3. If function is null, then:
    let Some(function) = function else {
        // 1. If throwIfMissing throw a TypeError.
        if throw_if_missing {
            return Err(Exception::type_error(
                "Trying to create a trusted type without a callback",
            ));
        }

        // 2. Else return null.
        return Ok(Value::NULL);
    };

    // 4. If trustedTypeName is "TrustedHTMLParserOptions":
    //    1. Set value to the result of creating a parser options callback argument given policy's relevant realm and
    //       value.
    let argument = create_a_parser_options_callback_argument(vm, realm, value)?;

    // 5. Let args be « value ».
    let args = MarkedVec::new(vm);
    args.push(argument);

    // 6. Append each item in arguments to args.
    args.extend_from_slice(arguments);

    // 7. Let policyValue be the result of invoking function with args and "rethrow".
    // 8. Return policyValue.
    Ok(function.invoke(None, &args.values())?)
}

// NB: A SetHTMLUnsafeOptions or ParseHTMLUnsafeOptions options needs vetting if options["runScripts"] exists and is
//     true, or if options["sanitizer"] exists and is not an empty SanitizerConfig.
//     https://w3c.github.io/trusted-types/dist/spec/#parser-options-need-vetting
fn needs_vetting(options: &SetHTMLUnsafeOptions) -> bool {
    if options.run_scripts {
        return true;
    }
    let Some(sanitizer) = &options.sanitizer else {
        return false;
    };
    // NOTE: An empty SanitizerConfig does not change the parsed markup.
    let SanitizerInput::SanitizerConfig(config) = sanitizer else {
        return true;
    };
    config.elements.is_some()
        || config.remove_elements.is_some()
        || config.replace_with_children_elements.is_some()
        || config.processing_instructions.is_some()
        || config.remove_processing_instructions.is_some()
        || config.attributes.is_some()
        || config.remove_attributes.is_some()
        || config.comments.is_some()
        || config.data_attributes.is_some()
        || config.javascript_ur_ls.is_some()
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-parser-options
// NB: Returns None when the algorithm returns input, which the caller holds.
pub(crate) fn get_trusted_type_compliant_parser_options(
    vm: &Vm,
    global: Gc<Object>,
    input: &SetHTMLUnsafeOptions,
    throw_if_missing: bool,
    sink: &Utf16String,
    options_from_author: bool,
) -> WebIdlResult<Option<Ref<TrustedHTMLParserOptions>>> {
    // 1. If input is an instance of TrustedHTMLParserOptions, return input.
    // NB: The caller returns such an input itself.

    // 2. Let requireTrustedTypes be the result of executing Does sink type require trusted types? algorithm, passing
    //    global, "script", and true.
    let script = Utf16String::from_utf8(SCRIPT_SINK_GROUP);
    let require_trusted_types = does_sink_require_trusted_types(global, &script, true);

    // 3. If requireTrustedTypes is false, return input.
    if !require_trusted_types {
        return Ok(None);
    }

    // 4. Let requirePolicy be throwIfMissing.
    let mut require_policy = throw_if_missing;

    // 5. If optionsFromAuthor is true and input needs vetting, set requirePolicy to true.
    if options_from_author && needs_vetting(input) {
        require_policy = true;
    }

    let reject_unless_allowed = || -> WebIdlResult<Option<Ref<TrustedHTMLParserOptions>>> {
        // 1. Let disposition be the result of executing should sink type mismatch violation be blocked by content
        //    security policy? algorithm, passing global, sink, "script", and the empty string as source.
        let blocked = should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
            global,
            sink,
            &script,
            &Utf16String::default(),
        );

        // 2. If disposition is "Allowed", return input.
        if !blocked {
            return Ok(None);
        }

        // 3. Throw a TypeError.
        Err(Exception::TypeError(utf16_formatted(
            "Sink {} requires parser options vetted by a Trusted Types policy",
            &[sink],
        )))
    };

    // 6. Let defaultPolicy be the value of global's trusted type policy factory's default policy.
    // 7. If defaultPolicy is null:
    let Some(default_policy) = trusted_types(global).default_policy_value() else {
        // 1. If requirePolicy is true:
        if require_policy {
            return reject_unless_allowed();
        }

        // 2. Return input.
        return Ok(None);
    };

    // 8. Let function be defaultPolicy's options["createParserOptions"].
    let function = default_policy.create_parser_options_callback();

    // 9. If function is null:
    if function.is_none() {
        // 1. If requirePolicy is true:
        if require_policy {
            return reject_unless_allowed();
        }

        // 2. Return input.
        return Ok(None);
    }

    // 10. Let argument be the result of creating a parser options callback argument given global's relevant realm and
    //     input.
    // 11. Let policyValue be the result of invoking function with « argument, "TrustedHTMLParserOptions", sink » and
    //     "rethrow".
    // 12. If the algorithm threw an error, rethrow the error and abort the following steps.
    let realm = global.shape().realm();
    let arguments = [
        Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_ascii(b"TrustedHTMLParserOptions"),
        )),
        Value::from_string(PrimitiveString::create(vm, sink.clone())),
    ];
    let policy_value = to_optional_set_html_unsafe_options(
        vm,
        get_parser_options_policy_value(vm, realm, function, input, &arguments, true)?,
    )?;

    // 13. If policyValue is null or undefined:
    let Some(policy_value) = policy_value else {
        // NOTE: This step assures that the default policy rejection will be reported, but ignored in a report-only
        //       mode.
        return reject_unless_allowed();
    };

    // 14. Let runScripts be policyValue["runScripts"], with default false.
    // NOTE: When invoked as a default policy, the createParserOptions callback determines the final script execution
    //       behavior, overriding the caller's requested runScripts option.
    let run_scripts = policy_value.run_scripts;

    // 15. Return a new TrustedHTMLParserOptions in global's relevant realm whose sanitizer configuration is set to the
    //     result of getting a sanitizer configuration from global's relevant realm and policyValue, and whose
    //     runScripts is set to runScripts.
    let sanitizer_config = get_a_sanitizer_config_copy_from_dictionary(vm, &policy_value)?;
    Ok(Some(TrustedHTMLParserOptions::create(
        vm,
        realm,
        sanitizer_config,
        run_scripts,
    )))
}

// https://w3c.github.io/trusted-types/dist/spec/#process-value-with-a-default-policy-algorithm
pub(crate) fn process_value_with_a_default_policy(
    vm: &Vm,
    trusted_type_name: TrustedTypeName,
    global: Gc<Object>,
    input: &TrustedTypeOrString,
    sink: &Utf16String,
) -> WebIdlResult<Option<TrustedType>> {
    // 1. Let defaultPolicy be the value of global’s trusted type policy factory’s default policy.
    let default_policy = trusted_types(global).default_policy_value();

    // This algorithm routes a value to be assigned to an injection sink through a default policy, should one exist.
    // FIXME: Open an issue upstream. It is not immediately clear what to do if the default policy does not exist.
    // Ref: https://github.com/w3c/trusted-types/issues/595
    let Some(default_policy) = default_policy else {
        return Ok(None);
    };

    // 2. Let policyValue be the result of executing Get Trusted Type policy value, with the following arguments:
    //    policy:
    //      defaultPolicy
    //    value:
    //      stringified input
    //    trustedTypeName:
    //      expectedType’s type name
    //    arguments:
    //      « trustedTypeName, sink »
    //    throwIfMissing:
    //      false
    //  3. If the algorithm threw an error, rethrow the error and abort the following steps.
    let arguments = [
        Value::from_string(PrimitiveString::create(
            vm,
            Utf16String::from_utf8(trusted_type_name.name()),
        )),
        Value::from_string(PrimitiveString::create(vm, sink.clone())),
    ];
    let policy_value =
        default_policy.get_trusted_type_policy_value(vm, trusted_type_name, &input.stringified(), &arguments, false)?;

    //  4. If policyValue is null or undefined, return policyValue.
    if policy_value.is_nullish() {
        return Ok(None);
    }

    //  5. Let dataString be the result of stringifying policyValue.
    let data_string = stringify(vm, trusted_type_name, policy_value)?;

    //  6. Return a new instance of an interface with a type name trustedTypeName, with its associated data value set to dataString.
    Ok(Some(TrustedType::create(vm, trusted_type_name, data_string)))
}

// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-string-algorithm
pub(crate) fn get_trusted_type_compliant_string(
    vm: &Vm,
    expected_type: TrustedTypeName,
    global: Gc<Object>,
    input: &TrustedTypeOrString,
    sink: &Utf16String,
    sink_group: &Utf16String,
) -> WebIdlResult<Utf16String> {
    // 1. If input is an instance of expectedType, return stringified input and abort these steps.
    if let TrustedTypeOrString::TrustedType(value) = input
        && value.type_name() == expected_type
    {
        return Ok(value.data().clone());
    }

    // 2. Let requireTrustedTypes be the result of executing Does sink type require trusted types? algorithm, passing global, sinkGroup, and true.
    let require_trusted_types = does_sink_require_trusted_types(global, sink_group, true);

    // 3. If requireTrustedTypes is false, return stringified input and abort these steps.
    if !require_trusted_types {
        return Ok(input.stringified());
    }

    // 4. Let convertedInput be the result of executing Process value with a default policy with the same arguments as this algorithm.
    // 5. If the algorithm threw an error, rethrow the error and abort the following steps.
    let converted_input = process_value_with_a_default_policy(vm, expected_type, global, input, sink)?;

    // 6. If convertedInput is null or undefined, execute the following steps:
    let Some(converted_input) = converted_input else {
        // 1. Let disposition be the result of executing Should sink type mismatch violation be blocked by Content Security Policy?
        //    algorithm, passing global, stringified input as source, sinkGroup and sink.
        let blocked = should_sink_type_mismatch_violation_be_blocked_by_content_security_policy(
            global,
            sink,
            sink_group,
            &input.stringified(),
        );

        // 2. If disposition is “Allowed”, return stringified input and abort further steps.
        if !blocked {
            return Ok(input.stringified());
        }

        // 3. Throw a TypeError and abort further steps.
        return Err(Exception::TypeError(utf16_formatted(
            "Sink {} of type {} requires a TrustedType to be used",
            &[sink, sink_group],
        )));
    };

    // 7. Assert: convertedInput is an instance of expectedType.
    assert!(converted_input.type_name() == expected_type);

    // 8. Return stringified convertedInput.
    Ok(converted_input.data().clone())
}

// https://w3c.github.io/trusted-types/dist/spec/#validate-attribute-mutation
pub(crate) fn get_trusted_types_compliant_attribute_value(
    vm: &Vm,
    attribute_name: &Utf16String,
    attribute_ns: Option<&Utf16String>,
    element_local_name: &Utf16String,
    element_namespace: Option<&Utf16String>,
    global: Gc<Object>,
    new_value: &TrustedTypeOrString,
) -> WebIdlResult<Utf16String> {
    // 1. If attributeNs is the empty string, set attributeNs to null.
    let attribute_ns = attribute_ns.filter(|attribute_ns| !attribute_ns.is_empty());

    // 2. Set attributeData to the result of Get Trusted Type data for attribute algorithm, with the following arguments:
    //    element
    //    attributeName
    //    attributeNs
    let attribute_data = get_trusted_type_data_for_attribute(
        &element_interface(element_local_name, element_namespace),
        attribute_name,
        attribute_ns,
    );

    // 3. If attributeData is null, then:
    let Some(attribute_data) = attribute_data else {
        // 1. If newValue is a string, return newValue.
        // 2. Assert: newValue is TrustedHTML or TrustedScript or TrustedScriptURL.
        // 3. Return value’s associated data.
        // FIXME: This is badly worded in the spec it should say "Return stringified newvalues's"
        return Ok(new_value.stringified());
    };

    // 4. Let expectedType be the value of the fourth member of attributeData.
    let expected_type = attribute_data.trusted_type;

    // 5. Let sink be the value of the fifth member of attributeData.
    let sink = attribute_data.sink;

    // 6. Return the result of executing Get Trusted Type compliant string with the following arguments:
    //      expectedType
    //      newValue as input
    //      element’s node document’s relevant global object as global
    //      sink
    //      'script' as sinkGroup
    get_trusted_type_compliant_string(
        vm,
        expected_type,
        global,
        new_value,
        &sink,
        &Utf16String::from_utf8(SCRIPT_SINK_GROUP),
    )
}

/// https://w3c.github.io/trusted-types/dist/spec/#trusted-types-sink-group
pub(crate) const SCRIPT_SINK_GROUP: &str = "'script'";

// FIXME: Add-hoc definition of an element interface
pub(crate) struct ElementInterface {
    pub(crate) element_name: &'static str,
    pub(crate) element_ns: Option<Utf16String>,
}

pub(crate) fn element_interface(local_name: &Utf16String, element_ns: Option<&Utf16String>) -> ElementInterface {
    // FIXME: We don't have a method in ElementFactory that can give us the interface name but these are all the cases
    // we care about in the table in get_trusted_type_data_for_attribute function
    let local_name_view = Utf16View::of_string(local_name);
    let element_ns_view = element_ns.map(Utf16View::of_string);
    let element_name = if local_name_view == "iframe" && element_ns_view.is_some_and(|ns| ns == HTML_NAMESPACE) {
        "HTMLIFrameElement"
    } else if local_name_view == "script" && element_ns_view.is_some_and(|ns| ns == HTML_NAMESPACE) {
        "HTMLScriptElement"
    } else if local_name_view == "script" && element_ns_view.is_some_and(|ns| ns == SVG_NAMESPACE) {
        "SVGScriptElement"
    } else {
        "Element"
    };
    ElementInterface {
        element_name,
        element_ns: element_ns.cloned(),
    }
}

/// What a Rust export of the algorithms tells C++: a string, a thrown value, or the message of a TypeError.
#[repr(u8)]
enum AlgorithmResultAbi {
    Normal = 0,
    Thrown = 1,
    TypeError = 2,
}

/// The result, written to `string` (a raw AK::Utf16String handed over) or `exception`.
///
/// # Safety
///
/// `string` and `exception` must be writable.
unsafe fn result_into_abi(result: WebIdlResult<Utf16String>, string: *mut usize, exception: *mut u64) -> u8 {
    // SAFETY: The caller passes writable pointers.
    unsafe {
        match result {
            Ok(value) => {
                *string = value.into_raw();
                AlgorithmResultAbi::Normal as u8
            }
            Err(Exception::Thrown(throw)) => {
                *exception = throw.value().to_raw();
                AlgorithmResultAbi::Thrown as u8
            }
            Err(Exception::TypeError(message)) => {
                *string = message.into_raw();
                AlgorithmResultAbi::TypeError as u8
            }
            Err(_) => unreachable!("the Trusted Types algorithms throw only TypeErrors"),
        }
    }
}

/// The TrustedType or string that C++ passes: a cell of the kind's interface, or a raw AK::Utf16String it hands over.
///
/// # Safety
///
/// `cell` must be a live cell of the interface of `kind` (TrustedTypeName + 1), or `string` a live raw string if `kind`
/// is 0.
unsafe fn trusted_type_or_string_from_abi(kind: u8, cell: *mut c_void, string: usize) -> TrustedTypeOrString {
    // SAFETY: The caller passes a live cell of the interface, or a live string.
    unsafe {
        match kind {
            0 => TrustedTypeOrString::String(Utf16String::from_raw_owned(string)),
            1 => TrustedTypeOrString::TrustedType(TrustedType::TrustedHTML(cell_from_abi(cell))),
            2 => TrustedTypeOrString::TrustedType(TrustedType::TrustedScript(cell_from_abi(cell))),
            3 => TrustedTypeOrString::TrustedType(TrustedType::TrustedScriptURL(cell_from_abi(cell))),
            _ => unreachable!("{kind} is not a kind of TrustedType or string"),
        }
    }
}

/// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-string-algorithm
///
/// # Safety
///
/// `vm` must be the live VM and `global` a live global object. `input_kind`, `input_cell` and `input_string` must be
/// as trusted_type_or_string_from_abi() takes them, `sink` and `sink_group` raw strings that C++ hands over, and
/// `string` and `exception` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_get_trusted_type_compliant_string(
    vm: *mut c_void,
    expected_type: u8,
    global: *mut c_void,
    input_kind: u8,
    input_cell: *mut c_void,
    input_string: usize,
    sink: usize,
    sink_group: usize,
    string: *mut usize,
    exception: *mut u64,
) -> u8 {
    // SAFETY: The caller passes the live VM, a live global object, a live input and live strings.
    let (vm, global, input, sink, sink_group) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new(global.cast::<Object>()).expect("C++ passes a global object")),
            trusted_type_or_string_from_abi(input_kind, input_cell, input_string),
            Utf16String::from_raw_owned(sink),
            Utf16String::from_raw_owned(sink_group),
        )
    };
    let result = get_trusted_type_compliant_string(
        vm,
        TrustedTypeName::from_abi(expected_type),
        global,
        &input,
        &sink,
        &sink_group,
    );
    // SAFETY: The caller passes writable pointers.
    unsafe { result_into_abi(result, string, exception) }
}

/// https://w3c.github.io/trusted-types/dist/spec/#validate-attribute-mutation
///
/// # Safety
///
/// As web_rust_trusted_types_get_trusted_type_compliant_string(), with `attribute_ns` a live raw string if
/// `has_attribute_ns`, `element_namespace` one if `has_element_namespace`, and the names raw strings that C++ hands
/// over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_get_trusted_types_compliant_attribute_value(
    vm: *mut c_void,
    attribute_name: usize,
    has_attribute_ns: bool,
    attribute_ns: usize,
    element_local_name: usize,
    has_element_namespace: bool,
    element_namespace: usize,
    global: *mut c_void,
    input_kind: u8,
    input_cell: *mut c_void,
    input_string: usize,
    string: *mut usize,
    exception: *mut u64,
) -> u8 {
    // SAFETY: The caller passes the live VM, a live global object, a live input and live strings.
    let (vm, global, input, attribute_name, attribute_ns, element_local_name, element_namespace) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new(global.cast::<Object>()).expect("C++ passes a global object")),
            trusted_type_or_string_from_abi(input_kind, input_cell, input_string),
            Utf16String::from_raw_owned(attribute_name),
            has_attribute_ns.then(|| Utf16String::from_raw_owned(attribute_ns)),
            Utf16String::from_raw_owned(element_local_name),
            has_element_namespace.then(|| Utf16String::from_raw_owned(element_namespace)),
        )
    };
    let result = get_trusted_types_compliant_attribute_value(
        vm,
        &attribute_name,
        attribute_ns.as_ref(),
        &element_local_name,
        element_namespace.as_ref(),
        global,
        &input,
    );
    // SAFETY: The caller passes writable pointers.
    unsafe { result_into_abi(result, string, exception) }
}

/// https://w3c.github.io/trusted-types/dist/spec/#process-value-with-a-default-policy-algorithm
///
/// # Safety
///
/// As web_rust_trusted_types_get_trusted_type_compliant_string(), with `result` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_process_value_with_a_default_policy(
    vm: *mut c_void,
    trusted_type_name: u8,
    global: *mut c_void,
    input_kind: u8,
    input_cell: *mut c_void,
    input_string: usize,
    sink: usize,
    result: *mut *mut c_void,
    exception: *mut u64,
) -> bool {
    // SAFETY: The caller passes the live VM, a live global object, a live input and a live string.
    let (vm, global, input, sink) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new(global.cast::<Object>()).expect("C++ passes a global object")),
            trusted_type_or_string_from_abi(input_kind, input_cell, input_string),
            Utf16String::from_raw_owned(sink),
        )
    };
    let converted =
        process_value_with_a_default_policy(vm, TrustedTypeName::from_abi(trusted_type_name), global, &input, &sink);
    // SAFETY: The caller passes writable pointers.
    unsafe {
        match converted {
            Ok(converted) => {
                *result = match converted {
                    None => core::ptr::null_mut(),
                    Some(TrustedType::TrustedHTML(value)) => cell_into_abi(value),
                    Some(TrustedType::TrustedScript(value)) => cell_into_abi(value),
                    Some(TrustedType::TrustedScriptURL(value)) => cell_into_abi(value),
                };
                false
            }
            Err(Exception::Thrown(throw)) => {
                *exception = throw.value().to_raw();
                true
            }
            Err(_) => unreachable!("process value with a default policy throws only what the policy throws"),
        }
    }
}

/// The parser options that C++ lends: the runScripts and the sanitizer input of a SetHTMLUnsafeOptions or
/// ParseHTMLUnsafeOptions dictionary.
#[repr(C)]
pub(crate) struct ParserOptionsInputAbi {
    run_scripts: bool,
    sanitizer_kind: SanitizerInputKind,
    sanitizer: *const c_void,
}

// NB: CompliantParserOptionsResult in TrustedTypePolicy.cpp matches this.
#[repr(u8)]
pub(crate) enum CompliantParserOptionsResultAbi {
    Input,
    TrustedHTMLParserOptions,
    Thrown,
    TypeError,
}

/// https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-parser-options
///
/// # Safety
///
/// `vm` must be the live VM and `global` a live global object. `input` and what it points to must be live for the call,
/// `sink` a raw string that C++ hands over, and `options`, `message` and `exception` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_trusted_types_get_trusted_type_compliant_parser_options(
    vm: *mut c_void,
    global: *mut c_void,
    input: &ParserOptionsInputAbi,
    throw_if_missing: bool,
    sink: usize,
    options_from_author: bool,
    options: &mut *mut c_void,
    message: &mut usize,
    exception: &mut u64,
) -> CompliantParserOptionsResultAbi {
    // SAFETY: The caller passes the live VM, a live global object and a sink string it hands over.
    let (vm, global, sink) = unsafe {
        (
            &*vm.cast::<Vm>(),
            Gc::from_non_null(NonNull::new(global.cast::<Object>()).expect("C++ passes a global object")),
            Utf16String::from_raw_owned(sink),
        )
    };
    let sanitizer = match input.sanitizer_kind {
        SanitizerInputKind::None => None,
        // SAFETY: C++ lends the sanitizer input of the kind for the call.
        kind => Some(unsafe { input_from_abi(kind, input.sanitizer) }),
    };
    // NB: A ParseHTMLUnsafeOptions has no runScripts member, which these algorithms read with default false.
    let input = SetHTMLUnsafeOptions {
        sanitizer,
        run_scripts: input.run_scripts,
    };
    match get_trusted_type_compliant_parser_options(vm, global, &input, throw_if_missing, &sink, options_from_author) {
        Ok(None) => CompliantParserOptionsResultAbi::Input,
        Ok(Some(trusted_options)) => {
            *options = cell_into_abi(trusted_options);
            CompliantParserOptionsResultAbi::TrustedHTMLParserOptions
        }
        Err(Exception::Thrown(throw)) => {
            *exception = throw.value().to_raw();
            CompliantParserOptionsResultAbi::Thrown
        }
        Err(Exception::TypeError(type_error)) => {
            *message = type_error.into_raw();
            CompliantParserOptionsResultAbi::TypeError
        }
        Err(_) => unreachable!("the Trusted Types algorithms throw only TypeErrors"),
    }
}
