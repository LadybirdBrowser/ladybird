// META: timeout=long
// META: script=/resources/WebIDLParser.js
// META: script=/resources/idlharness.js
// META: script=helpers.js

// https://w3c.github.io/webauthn/

'use strict';

idl_test(
  ['webauthn'],
  ['credential-management'],
  async idlArray => {
    // NOTE: The following are tested in idlharness-manual.https.window.js:
    // idlArray.add_objects({
    //   PublicKeyCredential: ['cred', 'assertion'],
    //   AuthenticatorAttestationResponse: ['cred.response'],
    //   AuthenticatorAssertionResponse: ['assertion.response']
    // });
  }
);
