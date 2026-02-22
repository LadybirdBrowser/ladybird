function throwInCatch() {
    let result = "bad";
    try {
        throw 1;
    } catch (e) {
        try {
            throw 2;
        } catch (e2) {
            result = "good";
        }
    }
    return result;
}
console.log(throwInCatch());

function throwInCatchWithLocals() {
    let outerVar = "outer";
    try {
        throw "first";
    } catch (e) {
        let catchLocal = "local:" + e;
        try {
            throw "second";
        } catch (inner) {
            return outerVar + "," + catchLocal + "," + inner;
        }
    }
}
console.log(throwInCatchWithLocals());
