import { createOscillatorSource } from "./oscillator.js";
import { createMediaElementSource } from "./media-element.js";
import { createMediaStreamSource } from "./media-stream.js";
import { createBufferSource } from "./buffer-source.js";
import { createConstantSource } from "./constant-source.js";
import { createGainProcessor } from "./gain.js";
import { createDelayProcessor } from "./delay.js";
import { createStereoPannerProcessor } from "./stereo-panner.js";
import { createPannerProcessor } from "./panner.js";
import { createBiquadFilterProcessor } from "./biquad-filter.js";
import { createWaveShaperProcessor } from "./wave-shaper.js";
import { createIIRFilterProcessor } from "./iir-filter.js";
import { createConvolverProcessor } from "./convolver.js";
import { createDynamicsCompressorProcessor } from "./dynamics-compressor.js";
import { createAnalyserProcessor } from "./analyser.js";
import { createOhNoesProcessor } from "./ohnoes.js";
import { createAudioWorkletProcessor } from "./audio-worklet.js";
import {
    clampNumber,
    createDefaultLogger,
    createFrequencyDomainSpectrum,
    createPillController,
    createTimeDomainOscilloscope,
    formatBitrate,
    formatBytes,
    parseNumber,
    safeConnect,
    safeDisconnect,
    setupMediaElementFilePicker,
    setupStatusTextLoop,
    syncRangeAndNumber,
} from "./webaudio.js";

const STORAGE_KEY = "webaudio-inspector-graph-v1";
const SAMPLE_RATE_KEY = "webaudio-sample-rate";

const log = createDefaultLogger();

const el = {
    graph: document.getElementById("graph"),
    btnToggle: document.getElementById("btnToggle"),
    sampleRateSelect: document.getElementById("sampleRateSelect"),
    stateDot: document.getElementById("stateDot"),
    stateText: document.getElementById("stateText"),
    status: document.getElementById("status"),
    logDot: document.getElementById("logDot"),
    logText: document.getElementById("logText"),
};

if (!el.graph) throw new Error("Missing #graph");
const setLogPill = createPillController({ dot: el.logDot, text: el.logText });
const setStatePill = createPillController({ dot: el.stateDot, text: el.stateText });

function loadSavedState() {
    try {
        const raw = window.localStorage.getItem(STORAGE_KEY);
        if (!raw) return null;
        return JSON.parse(raw);
    } catch (e) {
        log(`loadSavedState failed: ${e}`);
        return null;
    }
}

function saveState(state) {
    try {
        window.localStorage.setItem(STORAGE_KEY, JSON.stringify(state));
    } catch (e) {
        log(`saveState failed: ${e}`);
    }
}

function loadSavedSampleRate() {
    try {
        const raw = window.localStorage.getItem(SAMPLE_RATE_KEY);
        const value = raw ? Number(raw) : NaN;
        if (Number.isFinite(value) && value > 0) return value;
    } catch (e) {
        log(`loadSavedSampleRate failed: ${e}`);
    }
    return null;
}

function saveSampleRate(value) {
    try {
        window.localStorage.setItem(SAMPLE_RATE_KEY, String(value));
    } catch (e) {
        log(`saveSampleRate failed: ${e}`);
    }
}

function setSampleRateSelect(value) {
    if (!el.sampleRateSelect) return;
    el.sampleRateSelect.value = String(value);
}

let persistTimer = 0;
function schedulePersist(stateProvider) {
    if (persistTimer) window.clearTimeout(persistTimer);
    persistTimer = window.setTimeout(() => {
        try {
            const state = stateProvider();
            if (state) saveState(state);
        } catch (e) {
            log(`persist failed: ${e}`);
        }
    }, 50);
}

function h(tag, className, text) {
    const e = document.createElement(tag);
    if (className) e.className = className;
    if (text !== undefined) e.textContent = text;
    return e;
}

function createParamRow(labelText) {
    const row = h("div", "row");
    const label = h("label", null, labelText);
    row.append(label);
    return { row, label };
}

function createInfoRow(labelText) {
    const row = h("div", "row");
    row.append(h("label", null, labelText));
    const value = h("div", "value", "-");
    row.append(value);
    row.append(h("div", "hint-inline", ""));
    return { row, value };
}

function createRangeNumberRow(options) {
    const { label, range, number, onChange, map } = options;

    const { row } = createParamRow(label);
    const rangeEl = document.createElement("input");
    rangeEl.type = "range";
    Object.assign(rangeEl, range);

    const numberEl = document.createElement("input");
    numberEl.type = "number";
    Object.assign(numberEl, number);

    row.append(rangeEl);
    row.append(numberEl);

    if (map && map.rangeToNumber && map.numberToRange) {
        rangeEl.addEventListener("input", () => {
            numberEl.value = map.rangeToNumber(rangeEl.value);
            onChange();
        });
        numberEl.addEventListener("input", () => {
            rangeEl.value = map.numberToRange(numberEl.value);
            onChange();
        });
    } else {
        syncRangeAndNumber(rangeEl, numberEl, onChange);
    }

    return { row, rangeEl, numberEl };
}

function createSelectRow(options) {
    const { label, optionsList, onChange } = options;
    const { row } = createParamRow(label);
    const select = document.createElement("select");

    for (const opt of optionsList) {
        const o = document.createElement("option");
        o.value = opt.value;
        o.textContent = opt.text;
        if (opt.selected) o.selected = true;
        select.append(o);
    }

    select.addEventListener("change", () => onChange(select.value));

    row.append(select);
    row.append(h("div", "hint-inline", ""));

    return { row, select };
}

function createCheckboxRow(options) {
    const { label, checked, onChange, hint } = options;
    const row = h("div", "row");
    row.style.gridTemplateColumns = "140px 1fr 110px";

    row.append(h("label", null, label));

    const wrap = h("div", null);
    const cbLabel = h("label", null);
    cbLabel.style.display = "flex";
    cbLabel.style.gap = "10px";
    cbLabel.style.alignItems = "center";

    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.checked = !!checked;

    cbLabel.append(cb);
    wrap.append(cbLabel);

    row.append(wrap);
    row.append(h("div", "hint-inline", hint || ""));

    cb.addEventListener("change", () => onChange(cb.checked));

    return { row, cb };
}

function chooseMediaRow(options) {
    const row = h("div", "flex-row");
    const label = h("div", null, options.labelText || "");

    const controls = h("div", null);
    controls.style.display = "flex";
    controls.style.gap = "8px";
    controls.style.alignItems = "center";

    const buttons = [];
    if (options.buttons && Array.isArray(options.buttons)) {
        for (const spec of options.buttons) {
            const button = document.createElement("button");
            button.type = "button";
            button.textContent = spec.text || "";
            if (spec.className) button.className = spec.className;
            if (spec.onClick) button.addEventListener("click", spec.onClick);
            controls.append(button);
            buttons.push(button);
        }
    }

    row.append(label);
    row.append(controls);

    return { row, label, controls, buttons };
}

const nodeUi = {
    h,
    clampNumber,
    parseNumber,
    createInfoRow,
    createRangeNumberRow,
    createSelectRow,
    createCheckboxRow,
    chooseMediaRow,
    safeConnect,
    safeDisconnect,
    formatBytes,
    formatBitrate,
    setupMediaElementFilePicker,
    setupStatusTextLoop,
    createTimeDomainOscilloscope,
    createFrequencyDomainSpectrum,
};

function createNodeCard(options) {
    const card = h("div", "panel node-card");
    card.style.boxShadow = "none";
    card.style.background = "rgba(0, 0, 0, 0.12)";

    const hd = h("div", "hd");
    hd.style.background = "transparent";

    const title = h("div", "node-title");
    title.append(h("div", null, options.title));
    if (options.subtitle) title.append(h("div", "hint-inline", options.subtitle));

    const controls = h("div", "node-controls");
    if (options.controls) controls.append(options.controls);

    hd.append(title);
    hd.append(controls);

    const bd = h("div", "bd");

    card.append(hd);
    card.append(bd);

    return { card, bd, controls };
}

function createAddSeparator(options) {
    const sep = h("div", "graph-sep");
    const line = h("div", "line");
    const add = h("div", "add");
    const label = h("label", null, "Add:");

    const select = document.createElement("select");
    const placeholder = document.createElement("option");
    placeholder.value = "";
    placeholder.textContent = "(select)";
    placeholder.selected = true;
    placeholder.disabled = true;
    select.append(placeholder);

    for (const opt of options.items) {
        const o = document.createElement("option");
        o.value = opt.value;
        o.textContent = opt.text;
        select.append(o);
    }

    select.addEventListener("change", () => {
        const v = select.value;
        // Reset to placeholder after each pick.
        select.selectedIndex = 0;
        options.onPick(v);
    });

    add.append(label);
    add.append(select);

    sep.append(line);
    sep.append(add);

    return { sep, select };
}

function createInspector(options) {
    let ctx = null;
    let topologyFrozen = false;
    let topologyDirty = true;

    // During teardown/switching we must not rebuild the graph, otherwise we can
    // accidentally create/connect new nodes for the old source and then lose
    // the ability to disconnect them.

    let suppressGraphRebuild = false;

    const processorKinds = [
        { value: "gain", text: "GainNode" },
        { value: "delay", text: "DelayNode" },
        { value: "stereoPanner", text: "StereoPannerNode" },
        { value: "panner", text: "PannerNode" },
        { value: "biquadFilter", text: "BiquadFilterNode" },
        { value: "waveShaper", text: "WaveShaperNode" },
        { value: "iirFilter", text: "IIRFilterNode" },
        { value: "convolver", text: "ConvolverNode" },
        { value: "dynamicsCompressor", text: "DynamicsCompressorNode" },
        { value: "analyser", text: "AnalyserNode" },
        { value: "audioWorklet", text: "AudioWorkletNode" },
        { value: "ohnoes", text: "OhNoesNode" },
    ];

    const sourceKinds = [
        { value: "oscillator", text: "OscillatorNode" },
        { value: "mediaElement", text: "MediaElementAudioSourceNode" },
        { value: "mediaStream", text: "MediaStreamAudioSourceNode" },
        { value: "bufferSource", text: "AudioBufferSourceNode" },
        { value: "constantSource", text: "ConstantSourceNode" },
    ];

    const inspector = {
        withGraphRebuildSuppressed(fn) {
            suppressGraphRebuild = true;
            try {
                fn();
            } finally {
                suppressGraphRebuild = false;
            }
        },

        bindSourceGraphChanged() {
            const src = inspector.source;
            if (!src || !src.setOnGraphChanged) return;
            src.setOnGraphChanged(() => {
                if (suppressGraphRebuild) return;
                inspector.markTopologyDirty();
                inspector.rebuildConnections().catch(e => log(`rebuildConnections failed: ${e}`));
            });
        },
        bindProcessorGraphChanged(processor) {
            if (!processor || !processor.setOnGraphChanged) return;
            processor.setOnGraphChanged(() => {
                if (suppressGraphRebuild) return;
                inspector.markTopologyDirty();
                inspector.rebuildConnections().catch(e => log(`rebuildConnections failed: ${e}`));
            });
        },
        source: null,
        processors: [],
        addSelects: [],
        deleteButtons: [],
        markTopologyDirty() {
            topologyDirty = true;
        },

        clearTopologyDirty() {
            topologyDirty = false;
        },

        isTopologyDirty() {
            return topologyDirty;
        },

        serialize() {
            return {
                source: {
                    kind: inspector.source.kind,
                    state: inspector.source.serializeState ? inspector.source.serializeState() : null,
                },
                processors: inspector.processors.map(p => ({
                    kind: p.kind,
                    state: p.serializeState ? p.serializeState() : null,
                })),
            };
        },

        async restore(saved) {
            if (!saved || !saved.source) return;

            inspector.withGraphRebuildSuppressed(() => {
                inspector.teardownAudio();
                inspector.source.teardown();
                for (const p of inspector.processors) p.teardown();
                inspector.processors = [];

                inspector.source = createSourceInstance(saved.source.kind || "oscillator", log);
                inspector.bindSourceGraphChanged();
                inspector.source.setContext(ctx);
                try {
                    if (saved.source.state && inspector.source.applyState)
                        inspector.source.applyState(saved.source.state);
                } catch (e) {
                    log(`restore source failed: ${e}`);
                }

                for (const pSaved of saved.processors || []) {
                    try {
                        const inst = createProcessorInstance(pSaved.kind, log);
                        inst.setContext(ctx);
                        if (pSaved.state && inst.applyState) inst.applyState(pSaved.state);
                        inspector.bindProcessorGraphChanged(inst);
                        inspector.processors.push(inst);
                    } catch (e) {
                        log(`restore processor failed (${pSaved.kind}): ${e}`);
                    }
                }
            });

            await inspector.rebuildConnections();
            inspector.render();
            inspector.clearTopologyDirty();
        },

        render() {
            options.graph.innerHTML = "";
            inspector.addSelects = [];
            inspector.deleteButtons = [];

            // Source card
            {
                const src = inspector.source;
                const sourcePicker = document.createElement("select");
                {
                    const placeholder = document.createElement("option");
                    placeholder.value = "";
                    placeholder.textContent = "Source";
                    placeholder.disabled = true;
                    placeholder.selected = true;
                    sourcePicker.append(placeholder);

                    for (const opt of sourceKinds) {
                        const o = document.createElement("option");
                        o.value = opt.value;
                        o.textContent = opt.text;
                        if (src.kind === opt.value) o.selected = true;
                        sourcePicker.append(o);
                    }
                }

                sourcePicker.addEventListener("change", () => {
                    void inspector.trySwitchSource(sourcePicker.value);
                });

                const controls = h("div", null);
                controls.style.display = "flex";
                controls.style.alignItems = "center";
                controls.style.gap = "10px";
                controls.append(sourcePicker);

                const card = createNodeCard({ title: "", subtitle: src.title, controls });
                card.card.dataset.topologyControl = "1";
                card.bd.append(src.uiRoot);

                inspector.addSelects.push({ select: sourcePicker });

                options.graph.append(card.card);
            }

            // Separator + processors
            for (let i = 0; i <= inspector.processors.length; i++) {
                const sep = createAddSeparator({
                    items: processorKinds,
                    onPick: kind => {
                        void inspector.tryAddProcessorAt(i, kind);
                    },
                });

                sep.sep.dataset.topologyControl = "1";
                inspector.addSelects.push({ select: sep.select });
                options.graph.append(sep.sep);

                if (i === inspector.processors.length) break;

                const p = inspector.processors[i];
                const del = document.createElement("button");
                del.textContent = "Delete";
                del.className = "danger";
                del.addEventListener("click", () => {
                    void inspector.tryDeleteProcessorAt(i);
                });

                const controls = h("div", null);
                controls.style.display = "flex";
                controls.style.alignItems = "center";
                controls.style.gap = "10px";
                controls.append(del);

                const card = createNodeCard({ title: "Node", subtitle: p.title, controls });
                card.card.dataset.topologyControl = "1";
                card.bd.append(p.uiRoot);

                inspector.deleteButtons.push(del);

                options.graph.append(card.card);
            }

            inspector.updateTopologyFrozenUI();
        },

        async setContext(newCtx) {
            ctx = newCtx;
            inspector.source.setContext(ctx);
            for (const p of inspector.processors) p.setContext(ctx);
            await inspector.rebuildConnections();
            inspector.render();

            const running = !!ctx && ctx.state === "running";
            for (const p of inspector.processors) {
                if (p.setControlsDisabled) p.setControlsDisabled(running);
            }
        },

        setTopologyFrozen(frozen) {
            topologyFrozen = frozen;
            inspector.updateTopologyFrozenUI();
            inspector.updateAnalyserVisualizers();

            for (const p of inspector.processors) {
                if (p.setControlsDisabled) p.setControlsDisabled(frozen);
            }
        },

        onContextSuspended() {
            try {
                if (inspector.source.onContextSuspended) inspector.source.onContextSuspended();
            } catch (e) {
                log(`onContextSuspended failed: ${e}`);
            }

            for (const p of inspector.processors) {
                try {
                    if (p.onContextSuspended) p.onContextSuspended();
                } catch (e) {
                    log(`processor onContextSuspended failed: ${e}`);
                }
            }

            inspector.updateAnalyserVisualizers();
        },

        onContextResumed() {
            try {
                if (inspector.source.onContextResumed) inspector.source.onContextResumed();
            } catch (e) {
                log(`onContextResumed failed: ${e}`);
            }

            for (const p of inspector.processors) {
                try {
                    if (p.onContextResumed) p.onContextResumed();
                } catch (e) {
                    log(`processor onContextResumed failed: ${e}`);
                }
            }

            inspector.updateAnalyserVisualizers();
        },

        updateTopologyFrozenUI() {
            const frozen = topologyFrozen;

            const topologyControls = options.graph.querySelectorAll("[data-topology-control='1']");
            for (const e of topologyControls) {
                e.classList.toggle("topology-frozen", frozen);
            }

            for (const w of inspector.addSelects) {
                w.select.disabled = frozen;
            }
            for (const b of inspector.deleteButtons) {
                b.disabled = frozen;
            }
        },

        async rebuildConnections() {
            if (!ctx) return;

            // Disconnect everything we own.
            inspector.source.disconnectAll();
            for (const p of inspector.processors) p.disconnectAll();

            // Ensure audio nodes exist.
            try {
                await inspector.source.ensureAudio(ctx);
                for (const p of inspector.processors) await p.ensureAudio(ctx);
            } catch (e) {
                log(`ensureAudio failed: ${e}`);
                return;
            }

            // Connect linear chain: source -> processors -> destination

            let out = inspector.source.outputNode();

            try {
                for (const p of inspector.processors) {
                    const node = p.audioNode();
                    if (out) safeConnect(out, node);
                    out = p.outputNode();
                }

                if (out) safeConnect(out, ctx.destination);
            } catch (e) {
                log(`rebuildConnections failed: ${e}`);
                return;
            }

            inspector.updateAnalyserVisualizers();

            if (inspector.clearTopologyDirty) inspector.clearTopologyDirty();
        },

        updateAnalyserVisualizers() {
            const running = !!ctx && ctx.state === "running";
            for (const p of inspector.processors) {
                if (p.kind === "analyser") {
                    p.setVisualizersEnabled(running);
                }
            }
        },

        async tryAddProcessorAt(index, kind) {
            if (topologyFrozen) {
                log("Topology is frozen while running; stop first.");
                return;
            }
            const inst = createProcessorInstance(kind, log);
            inst.setContext(ctx);
            inspector.bindProcessorGraphChanged(inst);
            inspector.processors.splice(index, 0, inst);
            inspector.markTopologyDirty();
            await inspector.rebuildConnections();
            inspector.render();
            log(`Added ${inst.title} at ${index}`);
            schedulePersist(() => inspector.serialize());
        },

        async tryDeleteProcessorAt(index) {
            if (topologyFrozen) {
                log("Topology is frozen while running; stop first.");
                return;
            }
            const inst = inspector.processors[index];
            if (!inst) return;
            inst.teardown();
            inspector.processors.splice(index, 1);
            inspector.markTopologyDirty();
            await inspector.rebuildConnections();
            inspector.render();
            log(`Deleted processor at ${index}`);
            schedulePersist(() => inspector.serialize());
        },

        async trySwitchSource(kind) {
            if (topologyFrozen) {
                log("Topology is frozen while running; stop first.");
                inspector.render();
                return;
            }

            if (!kind || kind === inspector.source.kind) {
                inspector.render();
                return;
            }

            inspector.markTopologyDirty();
            inspector.withGraphRebuildSuppressed(() => {
                inspector.teardownAudio();
                inspector.source.teardown();
            });
            inspector.source = createSourceInstance(kind, log);
            inspector.bindSourceGraphChanged();
            inspector.source.setContext(ctx);
            await inspector.rebuildConnections();
            inspector.render();
            log(`Switched source -> ${inspector.source.title}`);
            schedulePersist(() => inspector.serialize());
        },

        teardownAudio() {
            for (const p of inspector.processors) p.teardownAudio();
            inspector.source.teardownAudio();
        },

        teardownAll() {
            inspector.teardownAudio();
            for (const p of inspector.processors) p.teardown();
            inspector.processors = [];
            inspector.source.teardown();
        },
    };

    inspector.source = createSourceInstance("oscillator", log);
    inspector.bindSourceGraphChanged();
    inspector.source.setContext(ctx);

    inspector.render();

    return inspector;
}

function createSourceInstance(kind, log) {
    if (kind === "oscillator") return createOscillatorSource(log, nodeUi);
    if (kind === "mediaElement") return createMediaElementSource(log, nodeUi);
    if (kind === "mediaStream") return createMediaStreamSource(log, nodeUi);
    if (kind === "bufferSource") return createBufferSource(log, nodeUi);
    if (kind === "constantSource") return createConstantSource(log, nodeUi);
    return createOscillatorSource(log, nodeUi);
}

function createProcessorInstance(kind, log) {
    if (kind === "gain") return createGainProcessor(log, nodeUi);
    if (kind === "delay") return createDelayProcessor(log, nodeUi);
    if (kind === "stereoPanner") return createStereoPannerProcessor(log, nodeUi);
    if (kind === "panner") return createPannerProcessor(log, nodeUi);
    if (kind === "biquadFilter") return createBiquadFilterProcessor(log, nodeUi);
    if (kind === "waveShaper") return createWaveShaperProcessor(log, nodeUi);
    if (kind === "iirFilter") return createIIRFilterProcessor(log, nodeUi);
    if (kind === "convolver") return createConvolverProcessor(log, nodeUi);
    if (kind === "dynamicsCompressor") return createDynamicsCompressorProcessor(log, nodeUi);
    if (kind === "analyser") return createAnalyserProcessor(log, nodeUi);
    if (kind === "audioWorklet") return createAudioWorkletProcessor(log, nodeUi);
    if (kind === "ohnoes") return createOhNoesProcessor(log, nodeUi);
    return createGainProcessor(log, nodeUi);
}
const inspector = createInspector({ graph: el.graph });

// Persist parameter tweaks.
el.graph?.addEventListener("input", () => schedulePersist(() => inspector.serialize()));
el.graph?.addEventListener("change", () => schedulePersist(() => inspector.serialize()));

let ctx = null;

const statusLoop = setupStatusTextLoop({
    target: el.status,
    intervalMs: 250,
    text: () => {
        try {
            const state = ctx ? ctx.state : "uninitialized";
            const t = ctx ? ctx.currentTime.toFixed(3) : "-";
            const sr = ctx ? ctx.sampleRate : "-";
            const q = ctx ? ctx.baseLatency : "-";
            return `state=${state}  currentTime=${t}  sampleRate=${sr}  baseLatency=${q}`;
        } catch (_) {
            return "status unavailable";
        }
    },
});

function updateTransportButton() {
    const running = !!ctx && ctx.state === "running";
    el.btnToggle.textContent = running ? "Stop" : "Start";
    el.btnToggle.classList.toggle("primary", !running);
    el.btnToggle.classList.toggle("danger", running);
}

function updateStateUI() {
    const state = ctx ? ctx.state : "uninitialized";
    setStatePill(state, ctx && state === "running" ? true : ctx ? null : false);
}

async function stopContext() {
    if (!ctx) return;
    try {
        await ctx.suspend();
    } catch (e) {
        log(`ctx.suspend() failed: ${e}`);
    }

    inspector.setTopologyFrozen(ctx.state === "running");
    if (ctx.state === "suspended") inspector.onContextSuspended();
    updateStateUI();
    updateTransportButton();
}

async function startContext() {
    if (!ctx) return;

    // Always rebuild before resuming so freshly added processors are wired even if
    // no parameter changes occurred while suspended.
    await inspector.rebuildConnections();
    if (inspector.isTopologyDirty && inspector.isTopologyDirty() && inspector.clearTopologyDirty)
        inspector.clearTopologyDirty();

    try {
        await ctx.resume();
    } catch (e) {
        log(`ctx.resume() failed: ${e}`);
    }

    inspector.setTopologyFrozen(ctx.state === "running");
    if (ctx.state === "running") inspector.onContextResumed();
    updateStateUI();
    updateTransportButton();
}

el.btnToggle.addEventListener("click", async () => {
    if (!ctx) return;
    if (ctx.state === "running") await stopContext();
    else await startContext();
});

async function initOnce() {
    if (ctx) return;

    const storedRate = loadSavedSampleRate();
    if (storedRate) {
        setSampleRateSelect(storedRate);
        await createContext(storedRate);
        return;
    }

    await createContext(null);
}

function getSelectedSampleRate() {
    const value = el.sampleRateSelect ? Number(el.sampleRateSelect.value) : NaN;
    if (Number.isFinite(value) && value > 0) return value;
    return 44100;
}

async function createContext(sampleRate) {
    if (ctx) return;

    try {
        const options = { latencyHint: "interactive" };
        if (Number.isFinite(sampleRate) && sampleRate > 0) options.sampleRate = sampleRate;
        ctx = new AudioContext(options);
    } catch (e) {
        ctx = null;
        log(`AudioContext creation failed: ${e}`);
        updateStateUI();
        updateTransportButton();
        return;
    }

    log(`Created AudioContext (state=${ctx.state}, sampleRate=${ctx.sampleRate})`);

    if (!Number.isFinite(sampleRate) || sampleRate <= 0) {
        setSampleRateSelect(ctx.sampleRate);
        saveSampleRate(ctx.sampleRate);
    }

    ctx.onstatechange = () => {
        log(`AudioContext statechange -> ${ctx.state}`);
        inspector.setTopologyFrozen(ctx.state === "running");
        if (ctx.state === "running") inspector.onContextResumed();
        if (ctx.state === "suspended") inspector.onContextSuspended();
        updateStateUI();
        updateTransportButton();
    };

    await inspector.setContext(ctx);

    // Ensure we start in suspended state.
    await stopContext();

    statusLoop.start();

    inspector.setTopologyFrozen(ctx.state === "running");
    updateStateUI();
    updateTransportButton();
}

async function recreateContext(sampleRate) {
    const savedState = inspector.serialize();
    if (ctx) {
        try {
            await ctx.close();
        } catch (e) {
            log(`ctx.close() failed: ${e}`);
        }
        ctx = null;
        updateStateUI();
        updateTransportButton();
    }

    await createContext(sampleRate);

    try {
        await inspector.restore(savedState);
    } catch (e) {
        log(`restore after context recreate failed: ${e}`);
    }
}

if (el.sampleRateSelect) {
    el.sampleRateSelect.addEventListener("change", async () => {
        const rate = getSelectedSampleRate();
        saveSampleRate(rate);
        await recreateContext(rate);
    });
}

// Restore any saved graph before kicking things off.
const saved = loadSavedState();
const restorePromise = saved
    ? inspector.restore(saved).catch(e => {
          log(`restore failed: ${e}`);
      })
    : Promise.resolve();

// Initial state.
inspector.setTopologyFrozen(true);
updateStateUI();
updateTransportButton();
setLogPill("ready", null);

initOnce()
    .then(() => restorePromise)
    .then(() => schedulePersist(() => inspector.serialize()));
