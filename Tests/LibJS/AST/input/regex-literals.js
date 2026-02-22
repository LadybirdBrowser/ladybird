// Basic regex literal
let a = /foo/;

// Regex with flags
let b = /bar/gi;

// Regex after operators (slash-forced-as-regex path)
let c = 1 + /baz/i;

// Regex in various expression positions
let d = [/abc/, /def/g];
let e = /hello world/;
let f = /[a-z]+/gms;

// Regex after keywords (also slash-forced-as-regex)
if (/test/.test("test")) {}
typeof /foo/;
void /foo/;
