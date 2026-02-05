import * as namespace from "./default-and-star-export.mjs";
import { "*" as fromStar } from "./default-and-star-export.mjs";
export { namespace };

import { namespace as nm } from "./re-export-namespace-via-binding.mjs";

export const passed = nm === namespace && fromStar === namespace["*"];
