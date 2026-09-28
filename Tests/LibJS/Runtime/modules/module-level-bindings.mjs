import * as namespaceImport from "./single-const-export.mjs";
import { foo as namespaceViaBinding } from "./namespace-re-export-star.mjs";

var variable = 1;
let lexical = 2;
const constant = 3;
let undefined = "shadowed undefined";

function declaredFunction() {
    return this;
}

class DeclaredClass {
    field = lexical;
    static staticField = constant;
    static {
        this.fromStaticBlock = variable;
    }
}

function readBindings() {
    return () => [variable, lexical, constant, undefined, typeof declaredFunction, declaredFunction()];
}

function writeBindings() {
    return () => {
        variable += 10;
        lexical += 20;
    };
}

export const readBeforeInitializationThrows = (() => {
    try {
        readLaterBinding();
    } catch (error) {
        return error instanceof ReferenceError;
    }
    return false;
})();

function readLaterBinding() {
    return laterBinding;
}

let laterBinding = "initialized";

export const constantAssignmentThrows = (() => {
    try {
        (() => {
            constant = 4;
        })();
    } catch (error) {
        return error instanceof TypeError;
    }
    return false;
})();

export const valuesBeforeWrite = readBindings()();
writeBindings()();
export const valuesAfterWrite = readBindings()();
export const laterBindingValue = readLaterBinding();
export const classValues = [new DeclaredClass().field, DeclaredClass.staticField, DeclaredClass.fromStaticBlock];
export const importedValues = [namespaceImport.passed, typeof namespaceViaBinding];

export default [variable, lexical].join();

export const passed = true;
