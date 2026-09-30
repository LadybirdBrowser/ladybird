const globalThisStr = getGlobalThisStr();

async_test(t => {
  globalThis.timeoutTrustedTest = t;
  let policy = createScript_policy(globalThis, 'timeout');
  let script = policy.createScript("globalThis.timeoutTrustedTest.done();");
  globalThis.setTimeout(script);
}, `${globalThisStr}.setTimeout assigned via policy (successful Script transformation).`);

async_test(t => {
  globalThis.intervalTrustedTest = t;
  let policy = createScript_policy(globalThis, 'script');
  let script = policy.createScript("globalThis.intervalTrustedTest.done();");
  globalThis.setInterval(script);
}, `${globalThisStr}.setInterval assigned via policy (successful Script transformation).`);

globalThis.trustedTypes.createPolicy("default", {createScript: (s, _, sink) => {
  // https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#timer-initialisation-steps,
  // step 9.6.1.1.
  const expectedSink = globalThisStr.includes("Window") ? "Window" : "WorkerGlobalScope";

  if (s === "timeoutStringTest") {
    assert_equals(sink, `${expectedSink} setTimeout`);
  } else if (s === "intervalStringTest") {
    assert_equals(sink, `${expectedSink} setInterval`);
  }
  return "globalThis." + s + ".done()";
}});

async_test(t => {
  globalThis.timeoutStringTest = t;
  let script = "timeoutStringTest";
  globalThis.setTimeout(script);
}, `${globalThisStr}.setTimeout assigned via default policy (successful Script transformation).`);

async_test(t => {
  globalThis.intervalStringTest = t;
  let script = "intervalStringTest";
  globalThis.setInterval(script);
}, `${globalThisStr}.setInterval assigned via default policy (successful Script transformation).`);

async_test(t => {
  globalThis.timeoutFunctionTest = t;
  let script = () => globalThis.timeoutFunctionTest.done();
  setTimeout(script);
}, `${globalThisStr}.setTimeout assigned with a function handler shouldn't go through default policy.`);

async_test(t => {
  globalThis.intervalFunctionTest = t;
  let script = () => globalThis.intervalFunctionTest.done();
  setInterval(script);
}, `${globalThisStr}.setInterval assigned with a function handler shouldn't go through default policy.`);
