function prefix(x) {
    return `prefix-${x}`;
}
function suffix(x) {
    return `${x}-suffix`;
}
function tostring(x) {
    return `${x}`;
}
function multi(a, b, c) {
    return `${a}${b}${c}`;
}
function literal() {
    return `hello world`;
}
function empty() {
    return ``;
}

const a = `abc` + "xyz";
const b = `abc` + `xyz`;
const c = "abc" + `xyz`;
const d = "abc" + "xyz";
const e = `abc` + `` + `xyz`;
const f = `abc` + "" + `xyz`;
const g = `${"abc"}${"xyz"}`;

const _prefix = prefix("abc");
const _suffix = suffix("abc");
const _tostring = tostring("abc");
const _multi = multi(1, 2, 3);
const _literal = literal();
const _empty = empty();
