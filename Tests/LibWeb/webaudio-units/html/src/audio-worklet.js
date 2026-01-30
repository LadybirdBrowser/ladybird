export function createAudioWorkletProcessor(log, ui) {
    const { h, createSelectRow, safeDisconnect } = ui;

    let ctx = null;
    let node = null;
    const modulePromises = new Map();
    let onGraphChanged = null;

    const processorOptions = [{ value: "passthru", text: "Passthru", script: "./processors/passthru.js" }];

    let selected = processorOptions[0];

    const uiRoot = h("div", null);
    const selectRow = createSelectRow({
        label: "Processor",
        optionsList: processorOptions.map(opt => ({
            value: opt.value,
            text: opt.text,
            selected: opt.value === selected.value,
        })),
        onChange: value => {
            const next = processorOptions.find(opt => opt.value === value) || processorOptions[0];
            if (next.value === selected.value) return;
            selected = next;
            if (node) {
                safeDisconnect(node);
                node = null;
            }
            if (onGraphChanged) onGraphChanged();
        },
    });

    uiRoot.append(selectRow.row);

    async function ensureModule(context, script) {
        const key = script;
        let promise = modulePromises.get(key);
        if (!promise) {
            const url = new URL(script, import.meta.url);
            promise = context.audioWorklet.addModule(url);
            modulePromises.set(key, promise);
        }
        await promise;
    }

    async function ensureAudio(context) {
        if (node) return;
        ctx = context;
        try {
            await ensureModule(context, selected.script);
            node = new AudioWorkletNode(context, selected.value);
        } catch (e) {
            node = null;
            log(`AudioWorklet failed: ${e}`);
        }
    }

    return {
        kind: "audioWorklet",
        title: "AudioWorkletNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        setOnGraphChanged(fn) {
            onGraphChanged = fn;
        },
        serializeState() {
            return { processor: selected.value };
        },
        applyState(state) {
            if (!state || !state.processor) return;
            const next = processorOptions.find(opt => opt.value === state.processor);
            if (!next) return;
            selected = next;
            selectRow.select.value = next.value;
        },
        ensureAudio,
        disconnectAll() {
            safeDisconnect(node);
        },
        audioNode() {
            if (!node) throw new Error("AudioWorklet node missing");
            return node;
        },
        outputNode() {
            if (!node) throw new Error("AudioWorklet node missing");
            return node;
        },
        teardownAudio() {
            safeDisconnect(node);
            node = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
