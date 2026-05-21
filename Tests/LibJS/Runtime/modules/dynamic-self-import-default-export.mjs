globalThis.dynamicSelfImportDefaultExportPassed = false;

export default (function () {
    return 99;
});

import("./dynamic-self-import-default-export.mjs").then(imported => {
    globalThis.dynamicSelfImportDefaultExportPassed = imported.default() === 99 && imported.default.name === "default";
});
