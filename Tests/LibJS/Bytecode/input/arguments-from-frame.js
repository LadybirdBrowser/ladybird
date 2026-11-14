function count(a, b) {
    return arguments.length;
}
function sloppy(a) {
    a = 1;
    let s = 0;
    for (let i = 0; i < arguments.length; ++i) s += arguments[i];
    return s + arguments[0];
}
function strict(a) {
    "use strict";
    return arguments[0] + arguments.length;
}
function writesParameter(a) {
    "use strict";
    a = 1;
    return arguments[0];
}
function beforeArrow(a) {
    const count = arguments.length;
    return () => count;
}
count();
beforeArrow();
sloppy();
strict();
writesParameter();
