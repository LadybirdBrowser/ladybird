import '../../resources/WebIDLParser.js';

export async function getEventHandlerAttributeWithInterfaceNames() {
  let attributeNamesWithInterfaceName = [];
  function isAttributeImplemented(interfaceName, name) {
    switch (interfaceName) {
      case 'GlobalEventHandlers':
        return name in HTMLElement.prototype;
      case 'WindowEventHandlers':
        return name in HTMLBodyElement.prototype;
      case 'HTMLMediaElement':
        return name in HTMLMediaElement.prototype;
      case 'SVGAnimationElement':
        return name in SVGAnimationElement.prototype;
      default:
       throw "Unknown interface";
    }
  }
  function addOnAttributes(IDL, interfaceName) {
    // Parsing the whole IDL file is slow, so use a small regexp to extract only
    // the part that is relevant for this test.
    let regexp = new RegExp(`^.*\(partial \)?interface \(mixin \)?${interfaceName}[^{]*{[^{}]*};$`, "m");
    let parsedIDL = WebIDL2.parse(IDL.match(regexp)[0]);
    parsedIDL.find(idl => idl.name === interfaceName)
      .members.map(member => member.name)
      .filter(name => name.length >= 3 && name.startsWith("on") &&
              !name.startsWith("onwebkit") &&
              isAttributeImplemented(interfaceName, name))
      .forEach(name => attributeNamesWithInterfaceName.push({name, interfaceName}));
  }
  const htmlIDL = await (await fetch("../interfaces/html.idl")).text();
  // GlobalEventHandlers exist on HTMLElement, SVGElement, and MathMLElement.
  // WindowEventHandlers exist on HTMLBodyElement, and HTMLFrameSetElement.
  ["GlobalEventHandlers", "WindowEventHandlers"].forEach(interfaceName => {
    addOnAttributes(htmlIDL, interfaceName);
  });

  const encryptedMediaIDL = await (await fetch("../interfaces/encrypted-media.idl")).text();
  // HTMLMediaElement (the parent for <audio> and <video>) has extra event handlers.
  addOnAttributes(encryptedMediaIDL, "HTMLMediaElement");

  const svgAnimationsIDL = await (await fetch("../interfaces/svg-animations.idl")).text();
  // SVGAnimationElement has extra event handlers.
  addOnAttributes(svgAnimationsIDL, "SVGAnimationElement");

  return attributeNamesWithInterfaceName;
}
