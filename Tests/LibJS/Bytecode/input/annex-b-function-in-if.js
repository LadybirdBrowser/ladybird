function annexBFunctionInIf(x) {
    if (x) {
        function inner() {
            return "true branch";
        }
    } else {
        function inner() {
            return "false branch";
        }
    }
    return inner();
}
console.log(annexBFunctionInIf(true));
console.log(annexBFunctionInIf(false));
