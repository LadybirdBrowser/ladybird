await Promise.resolve(0);

export const foo = "Well hello async shadows";

await 1;

export default "Default export";

await Promise.resolve(2);

export const bar = "'bar' export";
