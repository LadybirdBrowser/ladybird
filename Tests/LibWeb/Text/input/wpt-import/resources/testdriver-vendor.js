function elementCenter(element) {
    const boundingRect = element.getBoundingClientRect();
    return {
        x: boundingRect.left + boundingRect.width / 2,
        y: boundingRect.top + boundingRect.height / 2
    };
}

window.test_driver_internal.click = function(element) {
    const centerPoint = elementCenter(element);
    window.internals.click(centerPoint.x, centerPoint.y);
    return Promise.resolve();
};

window.test_driver_internal.send_keys = function(element, keys) {
    window.internals.sendText(element, keys);
    return Promise.resolve();
}

window.test_driver_internal.get_computed_label = async function(element) {
    return await window.internals.getComputedLabel(element);
};

window.test_driver_internal.get_computed_role = async function(element) {
    return await window.internals.getComputedRole(element);
};

window.test_driver_internal.delete_all_cookies = function(context) {
    return window.internals.deleteAllCookies();
};

const pointerStates = new Map();

window.test_driver_internal.action_sequence = async function(actions, context) {
    if (actions.some(source => source.type === "pointer" && ["touch", "pen"].includes(source.parameters?.pointerType))) {
        const converted = actions.map(source => ({ ...source, actions: source.actions.map(action => {
            if (!(action.origin instanceof Element))
                return action;
            const center = elementCenter(action.origin);
            return { ...action, origin: "viewport", x: Math.round(center.x + (action.x ?? 0)), y: Math.round(center.y + (action.y ?? 0)) };
        }) }));
        return window.internals.performActions(JSON.stringify({ actions: converted }));
    }

    // Modifier key codes from WebDriver spec
    const SHIFT = "\uE008";
    const CTRL = "\uE009";
    const ALT = "\uE00A";
    const META = "\uE03D";

    // Modifier flags matching Internals.idl
    const MOD_SHIFT = 4;
    const MOD_CTRL = 2;
    const MOD_ALT = 1;
    const MOD_SUPER = 8;

    let modifiers = 0;
    const target = document.activeElement;

    // The state of each mouse pointer source, which WebDriver keeps across action sequences.
    const pointerStateFor = source => {
        const id = source.id ?? "";
        if (!pointerStates.has(id))
            pointerStates.set(id, { x: 0, y: 0, lastUp: null, clickCount: 0 });
        return pointerStates.get(id);
    };

    const pointerActionPosition = (state, action) => {
        let x = action.x ?? 0;
        let y = action.y ?? 0;
        if (action.origin instanceof Element) {
            const centerPoint = elementCenter(action.origin);
            x += centerPoint.x;
            y += centerPoint.y;
        } else if (action.origin === "pointer") {
            x += state.x;
            y += state.y;
        }
        return { x, y };
    };

    const tickCount = Math.max(0, ...actions.map(source => source.actions ? source.actions.length : 0));
    for (let tick = 0; tick < tickCount; tick++) {
        for (const source of actions) {
            if (!source.actions || tick >= source.actions.length)
                continue;
            const action = source.actions[tick];

            if (source.type === "key") {
                if (action.type === "pause")
                    continue;
                const key = action.value;
                const isKeyDown = action.type === "keyDown";

                // Update modifier state
                if (key === SHIFT) {
                    modifiers = isKeyDown ? (modifiers | MOD_SHIFT) : (modifiers & ~MOD_SHIFT);
                } else if (key === CTRL) {
                    modifiers = isKeyDown ? (modifiers | MOD_CTRL) : (modifiers & ~MOD_CTRL);
                } else if (key === ALT) {
                    modifiers = isKeyDown ? (modifiers | MOD_ALT) : (modifiers & ~MOD_ALT);
                } else if (key === META) {
                    modifiers = isKeyDown ? (modifiers | MOD_SUPER) : (modifiers & ~MOD_SUPER);
                } else if (isKeyDown) {
                    // For non-modifier keys, only send on keyDown
                    window.internals.sendText(target, key, modifiers);
                }
                continue;
            }

            // Only mouse pointers can be driven through internals.
            if (source.type !== "pointer" || (source.parameters?.pointerType ?? "mouse") !== "mouse")
                continue;

            const state = pointerStateFor(source);
            if (action.type === "pointerMove") {
                const position = pointerActionPosition(state, action);
                state.x = position.x;
                state.y = position.y;
                window.internals.mouseMove(state.x, state.y, modifiers);
            } else if (action.type === "pointerDown") {
                const button = action.button ?? 0;
                // A press at the place of the last release with the same button continues its click sequence.
                const continuesClick = state.lastUp && state.lastUp.button === button && state.lastUp.x === state.x && state.lastUp.y === state.y;
                state.clickCount = continuesClick ? state.clickCount + 1 : 1;
                window.internals.mouseDown(state.x, state.y, state.clickCount, button, modifiers);
            } else if (action.type === "pointerUp") {
                const button = action.button ?? 0;
                window.internals.mouseUp(state.x, state.y, button, modifiers);
                state.lastUp = { x: state.x, y: state.y, button };
            }
        }
    }

    for (const source of actions) {
        if (source.type !== "wheel" || !source.actions) {
            continue;
        }
        for (const action of source.actions) {
            if (action.type !== "scroll") {
                continue;
            }
            let x = action.x;
            let y = action.y;
            if (action.origin instanceof Element) {
                const centerPoint = elementCenter(action.origin);
                x += centerPoint.x;
                y += centerPoint.y;
            }
            await window.internals.wheel(x, y, action.deltaX, action.deltaY);
        }
    }
};
