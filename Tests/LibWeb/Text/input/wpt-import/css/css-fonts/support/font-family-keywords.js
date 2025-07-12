// <generic-family> keywords, as specified in
// https://drafts.csswg.org/css-fonts/#generic-family-value
var kGenericFontFamilyKeywords = [
    "serif",
    "sans-serif",
    "cursive",
    "fantasy",
    "monospace",
    "system-ui",
    "math",
    "generic(fangsong)",
    "generic(kai)",
    "generic(khmer-mul)",
    "generic(nastaliq)",
    "ui-serif",
    "ui-sans-serif",
    "ui-monospace",
    "ui-rounded",
];

// <family-name> values that had/have web-exposed behavior in some browsers, but
// are not defined in the specification. They must be treated as <custom-ident>s.
var kNonGenericFontFamilyKeywords = [
    "NonGenericFontFamilyName",
    "-webkit-body",
    "-webkit-standard",
    "-webkit-pictograph",
    "emoji",
    "fangsong",
    "BlinkMacSystemFont",
];
