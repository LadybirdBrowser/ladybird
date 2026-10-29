(() => {
    // A page learns of gamepads only once it uses them and the user then interacts with one.
    globalThis.startUsingGamepads = () => {
        navigator.getGamepads();
    };

    globalThis.pumpGamepadEvents = () => {
        internals.pumpGamepadEvents();
        return new Promise(resolve => {
            setTimeout(() => resolve(), 0);
        });
    };

    // The activation of a gamepad user gesture comes over another connection than the pumped gamepad state, so it can
    // arrive after it.
    globalThis.waitForUserActivation = (targetWindow = window) =>
        waitForCondition(() => targetWindow.navigator.userActivation.isActive);

    globalThis.getStringifiedGamepads = () => {
        const gamepads = navigator.getGamepads().map(gamepad => ({
            id: gamepad.id,
            index: gamepad.index,
            connected: gamepad.connected,
            mapping: gamepad.mapping,
            axes: gamepad.axes,
            buttons: gamepad.buttons.map(button => ({
                pressed: button.pressed,
                touched: button.touched,
                value: button.value,
            })),
            vibrationActuator: {
                effects: gamepad.vibrationActuator.effects,
            },
        }));

        return JSON.stringify(gamepads);
    };

    globalThis.listenForGamepadConnected = () => {
        window.addEventListener("gamepadconnected", ({ gamepad }) => {
            println(`Received gamepadconnected event for '${gamepad.id}' at index ${gamepad.index}`);
        });
    };

    globalThis.listenForGamepadDisconnected = () => {
        window.addEventListener("gamepaddisconnected", ({ gamepad }) => {
            println(`Received gamepaddisconnected event for '${gamepad.id}' at index ${gamepad.index}`);
        });
    };
})();
