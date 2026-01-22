import * as ns from "./default-and-star-export.mjs";
import defaultExport from "./default-and-star-export.mjs";

export const passed =
    ns.default === "defaultValue" &&
    ns["*"] === "starExportValue" &&
    ns[""] === "empty" &&
    defaultExport === "defaultValue";
