import { "*" as starImport, "" as emptyImport } from "./default-and-star-export.mjs";

import {
    "*" as starImportIndirect,
    "" as emptyImportIndirect,
} from "./default-and-star-export-indirect.mjs";

export const passed =
    starImport === "starExportValue" &&
    starImportIndirect === "starExportValue" &&
    emptyImport === "empty" &&
    emptyImportIndirect === "empty";
