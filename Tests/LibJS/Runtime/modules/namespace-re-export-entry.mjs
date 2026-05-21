import { foo } from "./namespace-re-export-star.mjs";

export const passed = typeof foo === "object";
