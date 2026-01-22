import { value as importValue } from "./loop-self.mjs";

export const value = "loop de loop whooo";

export const passed = value === importValue;
