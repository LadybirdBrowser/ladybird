"use strict";

// Encodes |data| into base64url string. There is no '=' padding, and the
// characters '-' and '_' must be used instead of '+' and '/', respectively.
function base64urlEncode(data) {
  let result = btoa(data);
  return result.replace(/=+$/g, '').replace(/\+/g, "-").replace(/\//g, "_");
}

// Decode |encoded| using base64url decoding.
function base64urlDecode(encoded) {
  return atob(encoded.replace(/\-/g, "+").replace(/\_/g, "/"));
}

// Encodes a Uint8Array as a base64url string.
function uint8ArrayToBase64url(array) {
  return base64urlEncode(String.fromCharCode.apply(null, array));
}

// Encodes a Uint8Array to lowercase hex.
function uint8ArrayToHex(array) {
  const hexTable = '0123456789abcdef';
  let s = '';
  for (let i = 0; i < array.length; i++) {
    s += hexTable.charAt(array[i] >> 4);
    s += hexTable.charAt(array[i] & 15);
  }
  return s;
}

// Convert a EC signature from DER to a concatenation of the r and s parameters,
// as expected by the subtle crypto API.
function convertDERSignatureToSubtle(der) {
  let index = -1;
  const SEQUENCE = 0x30;
  const INTEGER = 0x02;
  assert_equals(der[++index], SEQUENCE);

  let size = der[++index];
  assert_equals(size + 2, der.length);

  assert_equals(der[++index], INTEGER);
  let rSize = der[++index];
  ++index;
  while (der[index] == 0) {
    ++index;
    --rSize;
  }
  let r = der.slice(index, index + rSize);
  index += rSize;

  assert_equals(der[index], INTEGER);
  let sSize = der[++index];
  ++index;
  while (der[index] == 0) {
    ++index;
    --sSize;
  }
  let s = der.slice(index, index + sSize);
  assert_equals(index + sSize, der.length);

  let result = new Uint8Array(64);
  result.set(r, 32 - rSize);
  result.set(s, 64 - sSize);
  return result;
};

function coseObjectToJWK(cose) {
  // Convert an object representing a COSE_Key encoded public key into a JSON
  // Web Key object.
  // https://tools.ietf.org/html/rfc7517

  // The example used on the test is a ES256 key, so we only implement that.
  let jwk = {};
  if (cose.type != 2)
    assert_unreached("Unknown type: " + cose.type);

  jwk.kty = "EC";
  if (cose.alg != ES256_ID)
    assert_unreached("Unknown alg: " + cose.alg);

  if (cose.crv != 1)
    assert_unreached("Unknown curve: " + jwk.crv);

  jwk.crv = "P-256";
  jwk.x = uint8ArrayToBase64url(cose.x);
  jwk.y = uint8ArrayToBase64url(cose.y);
  return jwk;
}

function parseCosePublicKey(coseKey) {
  // Parse a CTAP2 canonical CBOR encoding form key.
  // https://fidoalliance.org/specs/fido-v2.0-id-20180227/fido-client-to-authenticator-protocol-v2.0-id-20180227.html#ctap2-canonical-cbor-encoding-form
  let parsed = new Cbor(coseKey);
  let cbor = parsed.getCBOR();
  let key = {
    type: cbor[1],
    alg: cbor[3],
  };
  if (key.type != 2)
    assert_unreached("Unknown key type: " + key.type);

  key.crv = cbor[-1];
  key.x = new Uint8Array(cbor[-2]);
  key.y = new Uint8Array(cbor[-3]);
  return key;
}

function parseAttestedCredentialData(attestedCredentialData) {
  // Parse the attested credential data according to
  // https://w3c.github.io/webauthn/#attested-credential-data
  let aaguid = attestedCredentialData.slice(0, 16);
  let credentialIdLength = (attestedCredentialData[16] << 8)
                         + attestedCredentialData[17];
  let credentialId =
      attestedCredentialData.slice(18, 18 + credentialIdLength);
  let credentialPublicKey = parseCosePublicKey(
      attestedCredentialData.slice(18 + credentialIdLength,
                                   attestedCredentialData.length));

  return { aaguid, credentialIdLength, credentialId, credentialPublicKey };
}

function parseAuthenticatorData(authenticatorData) {
  // Parse the authenticator data according to
  // https://w3c.github.io/webauthn/#sctn-authenticator-data
  assert_greater_than_equal(authenticatorData.length, 37);
  let flags = authenticatorData[32];
  let counter = authenticatorData.slice(33, 37);

  let attestedCredentialData = authenticatorData.length > 37 ?
        parseAttestedCredentialData(authenticatorData.slice(37)) : null;
  let extensions = null;
  if (attestedCredentialData &&
      authenticatorData.length > 37 + attestedCredentialData.length) {
    extensions = authenticatorData.slice(37 + attestedCredentialData.length);
  }

  return {
    rpIdHash: authenticatorData.slice(0, 32),
    flags: {
      up: !!(flags & 0x01),
      uv: !!(flags & 0x04),
      at: !!(flags & 0x40),
      ed: !!(flags & 0x80),
    },
    counter: (counter[0] << 24)
           + (counter[1] << 16)
           + (counter[2] << 8)
           + counter[3],
    attestedCredentialData,
    extensions,
  };
}

// Taken from
// https://cs.chromium.org/chromium/src/chrome/browser/resources/cryptotoken/cbor.js?rcl=c9b6055cf9c158fb4119afd561a591f8fc95aefe
class Cbor {
  constructor(buffer) {
    this.slice = new Uint8Array(buffer);
  }
  get data() {
    return this.slice;
  }
  get length() {
    return this.slice.length;
  }
  get empty() {
    return this.slice.length == 0;
  }
  get hex() {
    return uint8ArrayToHex(this.data);
  }
  compare(other) {
    if (this.length < other.length) {
      return -1;
    } else if (this.length > other.length) {
      return 1;
    }
    for (let i = 0; i < this.length; i++) {
      if (this.slice[i] < other.slice[i]) {
        return -1;
      } else if (this.slice[i] > other.slice[i]) {
        return 1;
      }
    }
    return 0;
  }
  getU8() {
    if (this.empty) {
      throw('Cbor: empty during getU8');
    }
    const byte = this.slice[0];
    this.slice = this.slice.subarray(1);
    return byte;
  }
  skip(n) {
    if (this.length < n) {
      throw('Cbor: too few bytes to skip');
    }
    this.slice = this.slice.subarray(n);
  }
  getBytes(n) {
    if (this.length < n) {
      throw('Cbor: insufficient bytes in getBytes');
    }
    const ret = this.slice.subarray(0, n);
    this.slice = this.slice.subarray(n);
    return ret;
  }
  getCBORHeader() {
    const copy = new Cbor(this.slice);
    const a = this.getU8();
    const majorType = a >> 5;
    const info = a & 31;
    if (info < 24) {
      return [majorType, info, new Cbor(copy.getBytes(1))];
    } else if (info < 28) {
      const lengthLength = 1 << (info - 24);
      let data = this.getBytes(lengthLength);
      let value = 0;
      for (let i = 0; i < lengthLength; i++) {
        // Javascript has problems handling uint64s given the limited range of
        // a double.
        if (value > 35184372088831) {
          throw('Cbor: cannot represent CBOR number');
        }
        // Not using bitwise operations to avoid truncating to 32 bits.
        value *= 256;
        value += data[i];
      }
      switch (lengthLength) {
        case 1:
          if (value < 24) {
            throw('Cbor: value should have been encoded in single byte');
          }
          break;
        case 2:
          if (value < 256) {
            throw('Cbor: non-minimal integer');
          }
          break;
        case 4:
          if (value < 65536) {
            throw('Cbor: non-minimal integer');
          }
          break;
        case 8:
          if (value < 4294967296) {
            throw('Cbor: non-minimal integer');
          }
          break;
      }
      return [majorType, value, new Cbor(copy.getBytes(1 + lengthLength))];
    } else {
      throw('Cbor: CBOR contains unhandled info value ' + info);
    }
  }
  getCBOR() {
    const [major, value] = this.getCBORHeader();
    switch (major) {
      case 0:
        return value;
      case 1:
        return 0 - (1 + value);
      case 2:
        return this.getBytes(value);
      case 3:
        return this.getBytes(value);
      case 4: {
        let ret = new Array(value);
        for (let i = 0; i < value; i++) {
          ret[i] = this.getCBOR();
        }
        return ret;
      }
      case 5:
        if (value == 0) {
          return {};
        }
        let copy = new Cbor(this.data);
        const [firstKeyMajor] = copy.getCBORHeader();
        if (firstKeyMajor == 3) {
          // String-keyed map.
          let lastKeyHeader = new Cbor(new Uint8Array(0));
          let lastKeyBytes = new Cbor(new Uint8Array(0));
          let ret = {};
          for (let i = 0; i < value; i++) {
            const [keyMajor, keyLength, keyHeader] = this.getCBORHeader();
            if (keyMajor != 3) {
              throw('Cbor: non-string in string-valued map');
            }
            const keyBytes = new Cbor(this.getBytes(keyLength));
            if (i > 0) {
              const headerCmp = lastKeyHeader.compare(keyHeader);
              if (headerCmp > 0 ||
                  (headerCmp == 0 && lastKeyBytes.compare(keyBytes) >= 0)) {
                throw(
                    'Cbor: map keys in wrong order: ' + lastKeyHeader.hex +
                    '/' + lastKeyBytes.hex + ' ' + keyHeader.hex + '/' +
                    keyBytes.hex);
              }
            }
            lastKeyHeader = keyHeader;
            lastKeyBytes = keyBytes;
            ret[keyBytes.parseUTF8()] = this.getCBOR();
          }
          return ret;
        } else if (firstKeyMajor == 0 || firstKeyMajor == 1) {
          // Number-keyed map.
          let lastKeyHeader = new Cbor(new Uint8Array(0));
          let ret = {};
          for (let i = 0; i < value; i++) {
            let [keyMajor, keyValue, keyHeader] = this.getCBORHeader();
            if (keyMajor != 0 && keyMajor != 1) {
              throw('Cbor: non-number in number-valued map');
            }
            if (i > 0 && lastKeyHeader.compare(keyHeader) >= 0) {
              throw(
                  'Cbor: map keys in wrong order: ' + lastKeyHeader.hex + ' ' +
                  keyHeader.hex);
            }
            lastKeyHeader = keyHeader;
            if (keyMajor == 1) {
              keyValue = 0 - (1 + keyValue);
            }
            ret[keyValue] = this.getCBOR();
          }
          return ret;
        } else {
          throw('Cbor: map keyed by invalid major type ' + firstKeyMajor);
        }
      default:
        throw('Cbor: unhandled major type ' + major);
    }
  }
  parseUTF8() {
    return (new TextDecoder('utf-8')).decode(this.slice);
  }
}
