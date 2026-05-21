if (importedVarValue !== undefined) {
    throw new Error(`Expected importedVarValue to be undefined before evaluation, got ${importedVarValue}`);
}

try {
    importedVarValue = 0;
    throw new Error("Expected assignment to importedVarValue to throw");
} catch (error) {
    if (!(error instanceof TypeError)) throw error;
}

try {
    namespace.localValue;
    throw new Error("Expected namespace.localValue to be in the TDZ before evaluation");
} catch (error) {
    if (!(error instanceof ReferenceError)) throw error;
}

import { varValue as importedVarValue } from "./entry-self-import.mjs";
import * as namespace from "./entry-self-import.mjs";

export var varValue = 1;
export let localValue = 2;
