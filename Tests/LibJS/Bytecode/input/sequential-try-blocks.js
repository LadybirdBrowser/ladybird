function sequentialTryCatch() {
    let result = "";
    try {
        result += "a";
        throw 1;
    } catch (e) {
        result += "b";
    }
    try {
        result += "c";
        throw 2;
    } catch (e) {
        result += "d";
    }
    return result;
}
console.log(sequentialTryCatch());
