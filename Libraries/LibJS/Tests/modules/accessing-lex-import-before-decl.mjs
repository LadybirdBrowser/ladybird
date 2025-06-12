let passed = true;

try {
    importedLexVariable;
    passed = false;
} catch (e) {
    if (!(e instanceof ReferenceError))
        throw new Error("Expected importedLexVariable; to throw ReferenceError got " + e);
}

try {
    // Even though value is let, this should still throw TypeError because it is immutable!
    importedLexVariable = 0;
    passed = false;
} catch (e) {
    if (!(e instanceof TypeError))
        throw new Error("Expected importedLexVariable = 0; to throw TypeError got " + e);
}

import { value as importedLexVariable } from "./accessing-lex-import-before-decl.mjs";
export let value = 123;

export { passed };
