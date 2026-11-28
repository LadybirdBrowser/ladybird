const ES256_ID = -7;
const CHALLENGE = "climb the mountain";

const PUBLIC_KEY_RP = {
    id: window.location.hostname,
    name: "Example RP",
};

const PUBLIC_KEY_USER = {
    id: new TextEncoder().encode("123456789"),
    name: "madeline@example.com",
    displayName: "Madeline",
};

// ES256.
const PUBLIC_KEY_PARAMETERS =  [{
    type: "public-key",
    alg: ES256_ID,
}];

const AUTHENTICATOR_SELECTION_CRITERIA = {
    requireResidentKey: false,
    userVerification: "discouraged",
};

const MAKE_CREDENTIAL_OPTIONS = {
    challenge: new TextEncoder().encode(CHALLENGE),
    rp: PUBLIC_KEY_RP,
    user: PUBLIC_KEY_USER,
    pubKeyCredParams: PUBLIC_KEY_PARAMETERS,
    authenticatorSelection: AUTHENTICATOR_SELECTION_CRITERIA,
    excludeCredentials: [],
};
