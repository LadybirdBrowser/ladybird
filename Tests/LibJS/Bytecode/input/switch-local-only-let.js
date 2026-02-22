// Switch with let/const that only have local bindings should not
// create a lexical environment (CreateLexicalEnvironment).
function switchLocalOnly(x) {
    switch (x) {
        case 1:
            let a = 10;
            return a;
        case 2:
            const b = 20;
            return b;
        default:
            return 0;
    }
}
console.log(switchLocalOnly(1));
console.log(switchLocalOnly(2));
console.log(switchLocalOnly(3));
