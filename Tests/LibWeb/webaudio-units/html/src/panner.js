export function createPannerProcessor(log, ui) {
    const { h, createCheckboxRow, createRangeNumberRow, createSelectRow, clampNumber, safeConnect, safeDisconnect } =
        ui;

    let ctx = null;
    let input = null;
    let panner = null;
    let output = null;

    let bypass = false;

    let rememberedPanningModel = "HRTF";
    let rememberedDistanceModel = "inverse";

    let rememberedPositionX = 0;
    let rememberedPositionY = 0;
    let rememberedPositionZ = 0;

    let rememberedOrientationX = 1;
    let rememberedOrientationY = 0;
    let rememberedOrientationZ = 0;

    let rememberedRefDistance = 1;
    let rememberedMaxDistance = 1000;
    let rememberedRolloffFactor = 1;
    let rememberedConeInnerAngle = 360;
    let rememberedConeOuterAngle = 0;
    let rememberedConeOuterGain = 0;

    const uiRoot = h("div", null);

    const bypassRow = createCheckboxRow({
        label: "Bypass",
        checked: false,
        onChange: checked => {
            bypass = checked;
            apply();
        },
        hint: "",
    });

    const panningModelRow = createSelectRow({
        label: "Panning Model",
        optionsList: [
            { value: "HRTF", text: "HRTF", selected: true },
            { value: "equalpower", text: "equalpower" },
        ],
        onChange: v => {
            rememberedPanningModel = v;
            apply();
        },
    });

    const distanceModelRow = createSelectRow({
        label: "Distance Model",
        optionsList: [
            { value: "inverse", text: "inverse", selected: true },
            { value: "linear", text: "linear" },
            { value: "exponential", text: "exponential" },
        ],
        onChange: v => {
            rememberedDistanceModel = v;
            apply();
        },
    });

    const posXRow = createRangeNumberRow({
        label: "Position X (m)",
        range: { min: "-10", max: "10", value: "0", step: "0.1" },
        number: { min: "-10", max: "10", value: "0", step: "0.1" },
        onChange: () => {
            const v = Number(posXRow.numberEl.value);
            rememberedPositionX = Number.isFinite(v) ? clampNumber(v, -10, 10) : 0;
            apply();
        },
    });

    const posYRow = createRangeNumberRow({
        label: "Position Y (m)",
        range: { min: "-10", max: "10", value: "0", step: "0.1" },
        number: { min: "-10", max: "10", value: "0", step: "0.1" },
        onChange: () => {
            const v = Number(posYRow.numberEl.value);
            rememberedPositionY = Number.isFinite(v) ? clampNumber(v, -10, 10) : 0;
            apply();
        },
    });

    const posZRow = createRangeNumberRow({
        label: "Position Z (m)",
        range: { min: "-10", max: "10", value: "0", step: "0.1" },
        number: { min: "-10", max: "10", value: "0", step: "0.1" },
        onChange: () => {
            const v = Number(posZRow.numberEl.value);
            rememberedPositionZ = Number.isFinite(v) ? clampNumber(v, -10, 10) : 0;
            apply();
        },
    });

    const oriXRow = createRangeNumberRow({
        label: "Orientation X",
        range: { min: "-1", max: "1", value: "1", step: "0.05" },
        number: { min: "-1", max: "1", value: "1", step: "0.05" },
        onChange: () => {
            const v = Number(oriXRow.numberEl.value);
            rememberedOrientationX = Number.isFinite(v) ? clampNumber(v, -1, 1) : 1;
            apply();
        },
    });

    const oriYRow = createRangeNumberRow({
        label: "Orientation Y",
        range: { min: "-1", max: "1", value: "0", step: "0.05" },
        number: { min: "-1", max: "1", value: "0", step: "0.05" },
        onChange: () => {
            const v = Number(oriYRow.numberEl.value);
            rememberedOrientationY = Number.isFinite(v) ? clampNumber(v, -1, 1) : 0;
            apply();
        },
    });

    const oriZRow = createRangeNumberRow({
        label: "Orientation Z",
        range: { min: "-1", max: "1", value: "0", step: "0.05" },
        number: { min: "-1", max: "1", value: "0", step: "0.05" },
        onChange: () => {
            const v = Number(oriZRow.numberEl.value);
            rememberedOrientationZ = Number.isFinite(v) ? clampNumber(v, -1, 1) : 0;
            apply();
        },
    });

    const refDistanceRow = createRangeNumberRow({
        label: "Ref Distance (m)",
        range: { min: "0", max: "10", value: "1", step: "0.1" },
        number: { min: "0", max: "10", value: "1", step: "0.1" },
        onChange: () => {
            const v = Number(refDistanceRow.numberEl.value);
            rememberedRefDistance = Number.isFinite(v) ? clampNumber(v, 0, 10) : 1;
            apply();
        },
    });

    const maxDistanceRow = createRangeNumberRow({
        label: "Max Distance (m)",
        range: { min: "1", max: "2000", value: "1000", step: "1" },
        number: { min: "1", max: "2000", value: "1000", step: "1" },
        onChange: () => {
            const v = Number(maxDistanceRow.numberEl.value);
            rememberedMaxDistance = Number.isFinite(v) ? clampNumber(v, 1, 2000) : 1000;
            apply();
        },
    });

    const rolloffRow = createRangeNumberRow({
        label: "Rolloff Factor",
        range: { min: "0", max: "10", value: "1", step: "0.1" },
        number: { min: "0", max: "10", value: "1", step: "0.1" },
        onChange: () => {
            const v = Number(rolloffRow.numberEl.value);
            rememberedRolloffFactor = Number.isFinite(v) ? clampNumber(v, 0, 10) : 1;
            apply();
        },
    });

    const coneInnerRow = createRangeNumberRow({
        label: "Cone Inner (deg)",
        range: { min: "0", max: "360", value: "360", step: "1" },
        number: { min: "0", max: "360", value: "360", step: "1" },
        onChange: () => {
            const v = Number(coneInnerRow.numberEl.value);
            rememberedConeInnerAngle = Number.isFinite(v) ? clampNumber(v, 0, 360) : 360;
            apply();
        },
    });

    const coneOuterRow = createRangeNumberRow({
        label: "Cone Outer (deg)",
        range: { min: "0", max: "360", value: "0", step: "1" },
        number: { min: "0", max: "360", value: "0", step: "1" },
        onChange: () => {
            const v = Number(coneOuterRow.numberEl.value);
            rememberedConeOuterAngle = Number.isFinite(v) ? clampNumber(v, 0, 360) : 0;
            apply();
        },
    });

    const coneOuterGainRow = createRangeNumberRow({
        label: "Cone Outer Gain",
        range: { min: "0", max: "1", value: "0", step: "0.01" },
        number: { min: "0", max: "1", value: "0", step: "0.01" },
        onChange: () => {
            const v = Number(coneOuterGainRow.numberEl.value);
            rememberedConeOuterGain = Number.isFinite(v) ? clampNumber(v, 0, 1) : 0;
            apply();
        },
    });

    uiRoot.append(
        bypassRow.row,
        panningModelRow.row,
        distanceModelRow.row,
        posXRow.row,
        posYRow.row,
        posZRow.row,
        oriXRow.row,
        oriYRow.row,
        oriZRow.row,
        refDistanceRow.row,
        maxDistanceRow.row,
        rolloffRow.row,
        coneInnerRow.row,
        coneOuterRow.row,
        coneOuterGainRow.row
    );

    function ensureAudio(context) {
        if (panner) return;
        ctx = context;
        input = ctx.createGain();
        output = ctx.createGain();
        try {
            panner = ctx.createPanner();
        } catch (e) {
            panner = null;
            log(`createPanner failed: ${e}`);
            return;
        }
        apply();
    }

    function apply() {
        if (!input || !output || !panner) return;

        try {
            input.disconnect();
        } catch (_) {}
        try {
            panner.disconnect();
        } catch (_) {}

        if (bypass) {
            safeConnect(input, output);
        } else {
            safeConnect(input, panner);
            safeConnect(panner, output);

            const posX = clampNumber(rememberedPositionX, -10, 10);
            const posY = clampNumber(rememberedPositionY, -10, 10);
            const posZ = clampNumber(rememberedPositionZ, -10, 10);

            const oriX = clampNumber(rememberedOrientationX, -1, 1);
            const oriY = clampNumber(rememberedOrientationY, -1, 1);
            const oriZ = clampNumber(rememberedOrientationZ, -1, 1);

            const refDistance = clampNumber(rememberedRefDistance, 0, 10);
            const maxDistance = clampNumber(rememberedMaxDistance, Math.max(refDistance, 1), 2000);
            const rolloff = clampNumber(rememberedRolloffFactor, 0, 10);
            const coneInner = clampNumber(rememberedConeInnerAngle, 0, 360);
            const coneOuter = Math.max(coneInner, clampNumber(rememberedConeOuterAngle, 0, 360));
            const coneOuterGain = clampNumber(rememberedConeOuterGain, 0, 1);

            try {
                panner.panningModel = rememberedPanningModel;
            } catch (e) {
                log(`panner.panningModel set failed: ${e}`);
            }

            try {
                panner.distanceModel = rememberedDistanceModel;
            } catch (e) {
                log(`panner.distanceModel set failed: ${e}`);
            }

            try {
                panner.positionX.value = posX;
                panner.positionY.value = posY;
                panner.positionZ.value = posZ;
            } catch (e) {
                log(`panner position set failed: ${e}`);
            }

            try {
                panner.orientationX.value = oriX;
                panner.orientationY.value = oriY;
                panner.orientationZ.value = oriZ;
            } catch (e) {
                log(`panner orientation set failed: ${e}`);
            }

            try {
                panner.refDistance = refDistance;
            } catch (e) {
                log(`panner.refDistance set failed: ${e}`);
            }

            try {
                panner.maxDistance = maxDistance;
            } catch (e) {
                log(`panner.maxDistance set failed: ${e}`);
            }

            try {
                panner.rolloffFactor = rolloff;
            } catch (e) {
                log(`panner.rolloffFactor set failed: ${e}`);
            }

            try {
                panner.coneInnerAngle = coneInner;
                panner.coneOuterAngle = coneOuter;
                panner.coneOuterGain = coneOuterGain;
            } catch (e) {
                log(`panner cone set failed: ${e}`);
            }
        }

        const disabled = bypass;
        panningModelRow.select.disabled = disabled;
        distanceModelRow.select.disabled = disabled;
        posXRow.rangeEl.disabled = disabled;
        posXRow.numberEl.disabled = disabled;
        posYRow.rangeEl.disabled = disabled;
        posYRow.numberEl.disabled = disabled;
        posZRow.rangeEl.disabled = disabled;
        posZRow.numberEl.disabled = disabled;
        oriXRow.rangeEl.disabled = disabled;
        oriXRow.numberEl.disabled = disabled;
        oriYRow.rangeEl.disabled = disabled;
        oriYRow.numberEl.disabled = disabled;
        oriZRow.rangeEl.disabled = disabled;
        oriZRow.numberEl.disabled = disabled;
        refDistanceRow.rangeEl.disabled = disabled;
        refDistanceRow.numberEl.disabled = disabled;
        maxDistanceRow.rangeEl.disabled = disabled;
        maxDistanceRow.numberEl.disabled = disabled;
        rolloffRow.rangeEl.disabled = disabled;
        rolloffRow.numberEl.disabled = disabled;
        coneInnerRow.rangeEl.disabled = disabled;
        coneInnerRow.numberEl.disabled = disabled;
        coneOuterRow.rangeEl.disabled = disabled;
        coneOuterRow.numberEl.disabled = disabled;
        coneOuterGainRow.rangeEl.disabled = disabled;
        coneOuterGainRow.numberEl.disabled = disabled;
    }

    return {
        kind: "panner",
        title: "PannerNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                bypass,
                panningModel: rememberedPanningModel,
                distanceModel: rememberedDistanceModel,
                positionX: rememberedPositionX,
                positionY: rememberedPositionY,
                positionZ: rememberedPositionZ,
                orientationX: rememberedOrientationX,
                orientationY: rememberedOrientationY,
                orientationZ: rememberedOrientationZ,
                refDistance: rememberedRefDistance,
                maxDistance: rememberedMaxDistance,
                rolloffFactor: rememberedRolloffFactor,
                coneInner: rememberedConeInnerAngle,
                coneOuter: rememberedConeOuterAngle,
                coneOuterGain: rememberedConeOuterGain,
            };
        },
        applyState(state) {
            if (!state) return;
            bypass = !!state.bypass;
            rememberedPanningModel = state.panningModel || rememberedPanningModel;
            rememberedDistanceModel = state.distanceModel || rememberedDistanceModel;
            if (state.positionX !== undefined) rememberedPositionX = clampNumber(Number(state.positionX), -10, 10);
            if (state.positionY !== undefined) rememberedPositionY = clampNumber(Number(state.positionY), -10, 10);
            if (state.positionZ !== undefined) rememberedPositionZ = clampNumber(Number(state.positionZ), -10, 10);
            if (state.orientationX !== undefined)
                rememberedOrientationX = clampNumber(Number(state.orientationX), -1, 1);
            if (state.orientationY !== undefined)
                rememberedOrientationY = clampNumber(Number(state.orientationY), -1, 1);
            if (state.orientationZ !== undefined)
                rememberedOrientationZ = clampNumber(Number(state.orientationZ), -1, 1);
            if (state.refDistance !== undefined) rememberedRefDistance = clampNumber(Number(state.refDistance), 0, 10);
            if (state.maxDistance !== undefined)
                rememberedMaxDistance = clampNumber(Number(state.maxDistance), 1, 2000);
            if (state.rolloffFactor !== undefined)
                rememberedRolloffFactor = clampNumber(Number(state.rolloffFactor), 0, 10);
            if (state.coneInner !== undefined) rememberedConeInnerAngle = clampNumber(Number(state.coneInner), 0, 360);
            if (state.coneOuter !== undefined) rememberedConeOuterAngle = clampNumber(Number(state.coneOuter), 0, 360);
            if (state.coneOuterGain !== undefined)
                rememberedConeOuterGain = clampNumber(Number(state.coneOuterGain), 0, 1);

            panningModelRow.select.value = rememberedPanningModel;
            distanceModelRow.select.value = rememberedDistanceModel;
            posXRow.numberEl.value = String(rememberedPositionX);
            posYRow.numberEl.value = String(rememberedPositionY);
            posZRow.numberEl.value = String(rememberedPositionZ);
            oriXRow.numberEl.value = String(rememberedOrientationX);
            oriYRow.numberEl.value = String(rememberedOrientationY);
            oriZRow.numberEl.value = String(rememberedOrientationZ);
            refDistanceRow.numberEl.value = String(rememberedRefDistance);
            maxDistanceRow.numberEl.value = String(rememberedMaxDistance);
            rolloffRow.numberEl.value = String(rememberedRolloffFactor);
            coneInnerRow.numberEl.value = String(rememberedConeInnerAngle);
            coneOuterRow.numberEl.value = String(rememberedConeOuterAngle);
            coneOuterGainRow.numberEl.value = String(rememberedConeOuterGain);

            panningModelRow.select.dispatchEvent(new Event("change"));
            distanceModelRow.select.dispatchEvent(new Event("change"));
            posXRow.numberEl.dispatchEvent(new Event("input"));
            posYRow.numberEl.dispatchEvent(new Event("input"));
            posZRow.numberEl.dispatchEvent(new Event("input"));
            oriXRow.numberEl.dispatchEvent(new Event("input"));
            oriYRow.numberEl.dispatchEvent(new Event("input"));
            oriZRow.numberEl.dispatchEvent(new Event("input"));
            refDistanceRow.numberEl.dispatchEvent(new Event("input"));
            maxDistanceRow.numberEl.dispatchEvent(new Event("input"));
            rolloffRow.numberEl.dispatchEvent(new Event("input"));
            coneInnerRow.numberEl.dispatchEvent(new Event("input"));
            coneOuterRow.numberEl.dispatchEvent(new Event("input"));
            coneOuterGainRow.numberEl.dispatchEvent(new Event("input"));
            bypassRow.cb.checked = bypass;
            apply();
        },
        disconnectAll() {
            // Only detach from the outer graph; keep the internal input->panner->output wiring intact.
            safeDisconnect(output);
        },
        audioNode() {
            if (!input) throw new Error("PannerNode input missing");
            return input;
        },
        outputNode() {
            if (!output) throw new Error("PannerNode output missing");
            return output;
        },
        teardownAudio() {
            safeDisconnect(input);
            safeDisconnect(panner);
            safeDisconnect(output);
            input = null;
            panner = null;
            output = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
