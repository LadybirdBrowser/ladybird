# Adding a new IDL file

Ladybird's build system does a lot of work of turning the IDL from a Web spec into code, but there are a few things you'll need to do yourself.

For the sake of example, let's say you're wanting to add the `HTMLDetailsElement`.

1. Create `LibWeb/HTML/HTMLDetailsElement.idl` with the contents of the IDL section of the spec. In this case, that would be:
```webidl
[Exposed=Window]
interface HTMLDetailsElement : HTMLElement {
    [HTMLConstructor] constructor();

    [CEReactions] attribute boolean open;
};
```

2. Add the IDL file to [`LibWeb/idl_files.cmake`](../Libraries/LibWeb/idl_files.cmake):
    - Use `libweb_js_bindings(HTML/HTMLDetailsElement)` for top-level interfaces.
    - Use `libweb_support_idl(...)` for support IDL files that do not generate a top-level bindings class.

3. Forward declare the generated class in [`LibWeb/Forward.h`](../Libraries/LibWeb/Forward.h):
    - `HTMLDetailsElement` in its namespace.
