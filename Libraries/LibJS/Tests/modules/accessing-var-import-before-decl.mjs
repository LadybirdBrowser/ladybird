let passed = true;

if (importedVarValue !== undefined)
    throw new Error("Expected importedVarValue === undefined; got " + importedVarValue);

try {
    importedVarValue = 0;
    passed = false;
} catch (e) {
    if (!(e instanceof TypeError))
        throw new Error("Expected importedVarValue = 0; to throw TypeError got " + e);
}

import { value as importedVarValue } from "./accessing-var-import-before-decl.mjs";
export var value = 123;

export { passed };
