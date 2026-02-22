// Regex literal (direct and slash-forced-as-regex paths)
let a = /foo/;
let b = /bar/gi;
let c = 1 + /baz/i;

// Regex inside a function (exercises ownership transfer through FunctionPayload)
function matchDigits(s) {
    return /\d+/g.test(s);
}
matchDigits("abc123");
