// The viewer runs with an opaque origin, which has no storage. pdf.js keeps its preferences and view history in
// localStorage, so it gets one of its own that lasts as long as the viewer.
const items = new Map();
const storage = {
    getItem: key => items.get(String(key)) ?? null,
    setItem: (key, value) => void items.set(String(key), String(value)),
    removeItem: key => void items.delete(String(key)),
};
Object.defineProperty(window, "localStorage", { value: storage, configurable: true });
