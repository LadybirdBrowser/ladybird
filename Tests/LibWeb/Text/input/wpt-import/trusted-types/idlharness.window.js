// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js

idl_test(
    ['trusted-types'],
    ['html', 'dom'],
    idl_array => {
      idl_array.add_objects({
        TrustedTypePolicyFactory: ['window.trustedTypes'],
        TrustedTypePolicy: ['window.trustedTypes.createPolicy("SomeName", { createHTML: s => s })'],
        TrustedHTML: ['window.trustedTypes.createPolicy("SomeName1", { createHTML: s => s }).createHTML("A string")'],
        TrustedScript: ['window.trustedTypes.createPolicy("SomeName2", { createScript: s => s }).createScript("A string")'],
        TrustedScriptURL: ['window.trustedTypes.createPolicy("SomeName3", { createScriptURL: s => s }).createScriptURL("A string")'],
      });
    }
);
