let a = String.raw`foo\nbar`;

function tag(strings) {
    return strings[0];
}

let b = tag`hello`;

function chain(s) {
    return function (s2) {
        return s[0] + s2[0];
    };
}

let c = chain`hello``world`;
