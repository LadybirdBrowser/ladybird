import AppKit
import Foundation
@_exported import GfxCxx

// extension Ladybird {

func toNSRect(_ gfxR: Gfx.Rect) -> NSRect {
    return NSMakeRect(CGFloat(gfxR.x), CGFloat(gfxR.y), CGFloat(gfxR.width), CGFloat(gfxR.height))
}

func toNSSize(_ gfxS: Gfx.Size) -> NSSize {
    return NSMakeSize(CGFloat(gfxS.width), CGFloat(gfxS.height))
}

func toNSPoint(_ gfxP: Gfx.Point) -> NSPoint {
    return NSMakePoint(gfxP.x, gfxP.y)
}
func gfxColorToNsColor(_ color: Gfx.Color) -> NSColor {
    return NSColor(
        red: CGFloat(color.red) / 255.0,
        green: CGFloat(color.green) / 255.0,
        blue: CGFloat(color.blue) / 255.0,
        alpha: CGFloat(color.alpha) / 255.0
    )
}

func toNSColor() {

}

extension NSRect {
    func toGfxRect() -> Gfx.Rect {
        return Gfx.Rect(x: Int(self.origin.x), y: Int(self.origin.y), width: Int(self.width), height: Int(self.height))
    }
}

extension NSPoint {
    func toGfxPoint() -> Gfx.Point {
        return Gfx.Point(x: Int(self.origin.x), y: Int(self.origin.y))
    }
}

extension NSSize {
    func toGfxSize() -> Gfx.Size {
        return Gfx.Size(width: Int(self.width), height: Int(self.height))
    }
}

extension NSColor {
    func toGfxColor() -> Gfx.Color? {
        guard let rgbColor = color.usingColorSpace(.genericRGB) else {
            return nil
        }

        return GfxColor(
            red: UInt8(rgbColor.redComponent * 255),
            green: UInt8(rgbColor.greenComponent * 255),
            blue: UInt8(rgbColor.blueComponent * 255),
            alpha: UInt8(rgbColor.alphaComponent * 255)
        )
    }
}

extension NSString {
    func toString() -> String {
        return self as String
    }

    func toByteString() -> Data {
        return Data(bytes: string.utf8String!, count: strlen(string.utf8String!))
    }
}

extension String {
	func toNSString() -> NSString {
		return self as NSString;
	}

	func toNSData() -> Data {
		return Data(string.utf8)
	}
}

func deserialize_json_to_dictionary(_ json: String) -> [String: Any]? {
    guard let jsonData = json.data(using: .utf8) else { return nil }

    do {
        if let dictionary = try JSONSerialization.jsonObject(with: jsonData, options: []) as? [String: Any] {
            return dictionary
        }
    } catch {
        print("Error deserializing DOM tree: \(error)")
    }

    return nil
}

// } // extension Ladybird
