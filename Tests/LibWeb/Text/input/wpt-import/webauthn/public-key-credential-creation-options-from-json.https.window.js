// META: script=/resources/testdriver.js
// META: script=/resources/testdriver-vendor.js
// META: script=/resources/utils.js
// META: script=helpers.js

// The string "test" as ASCII bytes and base64url-encoded.
const test_bytes = new Uint8Array([0x74, 0x65, 0x73, 0x74]);
const test_b64 = "dGVzdA";

test(() => {
  let actual = PublicKeyCredential.parseCreationOptionsFromJSON({
    rp: {
      id: "example.com",
      name: "Example Inc",
    },
    user: {
      id: test_b64,
      name: "test@example.com",
      displayName: "test user"
    },
    challenge: test_b64,
    pubKeyCredParams: [
      {
        type: "public-key",
        alg: -7,
      },
    ],
  });
  let expected = {
    rp: {
      id: "example.com",
      name: "Example Inc",
    },
    user: {
      id: test_bytes,
      name: "test@example.com",
      displayName: "test user"
    },
    challenge: test_bytes,
    pubKeyCredParams: [
      {
        type: "public-key",
        alg: -7,
      },
    ],
    // The spec defaults the following fields:
    attestation: "none",
    hints: [],
  };

  assertJsonEquals(actual.rp, expected.rp);
  assert_true(userEntityEquals(actual.user, expected.user));
  assert_true(bytesEqual(actual.challenge, expected.challenge));
  assertJsonEquals(actual.pubKeyCredParams, expected.pubKeyCredParams, "pk");
  assert_equals(actual.attestation, expected.attestation);
  if (actual.hasOwnProperty('hints')) {
    // Not all implementations support hints yet.
    assertJsonEquals(actual.hints, expected.hints);
  }
}, "parseCreationOptionsFromJSON()");

test(() => {
  assert_throws_dom("EncodingError", () => {
    PublicKeyCredential.parseCreationOptionsFromJSON({
      rp: {
        id: "example.com",
        name: "Example Inc",
      },
      user: {
        id: "not valid base64url",
        name: "test@example.com",
        displayName: "test user"
      },
      challenge: "not valid base64url",
      pubKeyCredParams: [
        {
          type: "public-key",
          alg: -7,
        },
      ],
    });
  });
}, "parseCreationOptionsFromJSON() throws EncodingError");

test(() => {
  let actual = PublicKeyCredential.parseCreationOptionsFromJSON({
    rp: {
      id: "example.com",
      name: "Example Inc",
    },
    user: {
      id: test_b64,
      name: "test@example.com",
      displayName: "test user"
    },
    challenge: test_b64,
    pubKeyCredParams: [
      {
        type: "public-key",
        alg: -7,
      },
    ],
    extensions: {
      appidExclude: "https://example.com/appid",
      hmacCreateSecret: true,
      credentialProtectionPolicy: "userVerificationRequired",
      enforceCredentialProtectionPolicy: true,
      minPinLength: true,
      credProps: true,
      largeBlob: {
        support: "required",
        write: test_b64,
      },
      credBlob: test_b64,
      prf: {
        eval: {
          first: test_b64,
          second: test_b64,
        },
        evalByCredential: {
          "test cred": {
            first: test_b64,
            second: test_b64,
          },
        },
      },
    },
  });
  let expected = {
    rp: {
      id: "example.com",
      name: "Example Inc",
    },
    user: {
      id: test_bytes,
      name: "test@example.com",
      displayName: "test user"
    },
    challenge: test_bytes,
    pubKeyCredParams: [
      {
        type: "public-key",
        alg: -7,
      },
    ],
    extensions: {
      appidExclude: "https://example.com/appid",
      hmacCreateSecret: true,
      credentialProtectionPolicy: "userVerificationRequired",
      enforceCredentialProtectionPolicy: true,
      minPinLength: true,
      credProps: true,
      largeBlob: {
        support: "required",
        write: test_bytes,
      },
      credBlob: test_bytes,
      prf: {
        eval: {
          first: test_bytes,
          second: test_bytes,
        },
        evalByCredential: {
          "test cred": {
            first: test_bytes,
            second: test_bytes,
          },
        },
      },
    },
  };

  // Some implementations do not support all of these extensions.
  if (actual.extensions.hasOwnProperty('appidExclude')) {
    assert_equals(
        actual.extensions.appidExclude, expected.extensions.appidExclude);
  }
  if (actual.extensions.hasOwnProperty('hmacCreateSecret')) {
    assert_equals(
        actual.extensions.hmacCreateSecret,
        expected.extensions.hmacCreateSecret);
  }
  if (actual.extensions.hasOwnProperty('credentialProtectionPolicy')) {
    assert_equals(
        actual.extensions.credentialProtectionPolicy,
        expected.extensions.credentialProtectionPolicy);
  }
  if (actual.extensions.hasOwnProperty('enforceCredentialProtectionPolicy')) {
    assert_equals(
        actual.extensions.enforceCredentialProtectionPolicy,
        expected.extensions.enforceCredentialProtectionPolicy);
  }
  if (actual.extensions.hasOwnProperty('minPinLength')) {
    assert_equals(
        actual.extensions.minPinLength, expected.extensions.minPinLength);
  }
  if (actual.extensions.hasOwnProperty('credProps')) {
    assert_equals(actual.extensions.credProps, expected.extensions.credProps);
  }
  if (actual.extensions.hasOwnProperty('largeBlob')) {
    assert_equals(
        actual.extensions.largeBlob.support,
        expected.extensions.largeBlob.support);
    assert_true(bytesEqual(
        actual.extensions.largeBlob.write,
        expected.extensions.largeBlob.write));
  }
  if (actual.extensions.hasOwnProperty('credBlob')) {
    assert_true(
        bytesEqual(actual.extensions.credBlob, expected.extensions.credBlob));
  }
  if (actual.extensions.hasOwnProperty('prf')) {
    let prfValuesEquals = (a, b) => {
      return bytesEqual(a.first, b.first) && bytesEqual(a.second, b.second);
    };
    assert_true(
        prfValuesEquals(
            actual.extensions.prf.eval, expected.extensions.prf.eval),
        'prf eval');
    assert_true(
        prfValuesEquals(
            actual.extensions.prf.evalByCredential['test cred'],
            expected.extensions.prf.evalByCredential['test cred']),
        'prf ebc');
  }
}, "parseCreationOptionsFromJSON() with extensions");

test(() => {
  let actual = PublicKeyCredential.parseCreationOptionsFromJSON({
    rp: {
      id: "example.com",
      name: "Example Inc",
    },
    user: {
      id: test_b64,
      name: "test@example.com",
      displayName: "test user"
    },
    challenge: test_b64,
    pubKeyCredParams: [
      {
        type: "public-key",
        alg: -7,
      },
    ],
    hints: ["duplicated-value", "duplicated-value", "hybrid", "hybrid"]
  });
  let expected = {
    rp: {
      id: "example.com",
      name: "Example Inc",
    },
    user: {
      id: test_bytes,
      name: "test@example.com",
      displayName: "test user"
    },
    challenge: test_bytes,
    pubKeyCredParams: [
      {
        type: "public-key",
        alg: -7,
      },
    ],
    attestation: "none",
    hints: ["duplicated-value", "duplicated-value", "hybrid", "hybrid"]
  };

  assert_equals(actual.attestation, expected.attestation);
  if (actual.hasOwnProperty('hints')) {
    // Not all implementations support hints yet.
    assertJsonEquals(actual.hints, expected.hints);
  }
}, "parseCreationOptionsFromJSON() with duplicated hints");
