const globalThisStr = getGlobalThisStr();

let compilationSink = null;
function resetSinkName() { compilationSink = null; }

trustedTypes.createPolicy("default", { createScript: (s, _, sink) => {
  compilationSink = sink;
  return `${s} // modified`;
}});

test(t => {
  t.add_cleanup(resetSinkName);
  assert_throws_js(EvalError, _ => eval("'42'"));
  assert_equals(compilationSink, "eval");
}, `Blocked eval in ${globalThisStr}.`);

test(t => {
  t.add_cleanup(resetSinkName);
  assert_throws_js(EvalError, _ => eval?.("'42'"));
  assert_equals(compilationSink, "eval");
}, `Blocked indirect eval in ${globalThisStr}.`);

const AsyncFunction = async function() {}.constructor;
const GeneratorFunction = function*() {}.constructor;
const AsyncGeneratorFunction = async function*() {}.constructor;

[Function, AsyncFunction, GeneratorFunction, AsyncGeneratorFunction].forEach(functionConstructor => {
  test(t => {
    t.add_cleanup(resetSinkName);
    assert_throws_js(EvalError, _ => new functionConstructor("return;"));
    assert_equals(compilationSink, "Function");
  }, `Blocked ${functionConstructor.name} constructor in ${globalThisStr}.`);
});
