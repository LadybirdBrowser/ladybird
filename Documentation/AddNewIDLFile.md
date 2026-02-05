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

2. If the IDL refers to other IDL types, you need to import those. For example, `CSSRule` has an attribute that returns a `CSSStyleSheet`, so that needs to be imported:
```webidl
#import <CSS/CSSStyleSheet.idl>

interface CSSRule {
    readonly attribute CSSStyleSheet? parentStyleSheet;
};
```

3. Add a `libweb_js_bindings(HTML/HTMLDetailsElement)` call to [`LibWeb/idl_files.cmake`](../Libraries/LibWeb/idl_files.cmake) (only for top-level interfaces)

4. Forward declare the generated class in [`LibWeb/Forward.h`](../Libraries/LibWeb/Forward.h):
    - `HTMLDetailsElement` in its namespace.

5. If your type isn't an Event or Element, you will need to add it to [`is_platform_object()`](../Meta/Lagom/Tools/CodeGenerators/LibWeb/BindingsGenerator/IDLGenerators.cpp)
   so that it can be accepted as an IDL parameter, attribute or return type.
