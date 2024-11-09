try {
    f();
} catch (e) {
    if (!(e instanceof ReferenceError)) throw e;
    if (!e.message.includes("bindingUsedInFunction")) throw e;
}

let bindingUsedInFunction = 0;

const immediateResult = f();
const immediateName = f.name + "";

import f from "./anon-func-decl-default-export.mjs";
export default function () {
    return bindingUsedInFunction++;
}

const postImportResult = f();
const postImportName = f.name + "";

export const passed =
    immediateResult === 0 &&
    postImportResult === 1 &&
    bindingUsedInFunction === 2 &&
    immediateName === "default" &&
    postImportName === "default";
