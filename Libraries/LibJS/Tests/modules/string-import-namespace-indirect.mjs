import * as indirectNs from "./default-and-star-export-indirect.mjs";
// This is an all-but-default export and should thus only provide "*" and "".
// default-and-star-export-indirect.mjs:
// export * from "./default-and-star-export.mjs";

import * as indirectNsString from "./default-and-star-export-indirect-string.mjs";
// This is an all export and should thus provide the full namespace.
// default-and-star-export-indirect-string.mjs:
// export * as "viaString" from "./default-and-star-export.mjs";

export const passed =
    indirectNs["*"] === "starExportValue" &&
    indirectNs[""] === "empty" &&
    indirectNs.default === undefined &&
    indirectNsString.viaString["*"] === "starExportValue" &&
    indirectNsString.viaString[""] === "empty" &&
    indirectNsString.viaString.default === "defaultValue";
