import Foundation
@_exported import GfxCxx

extension Gfx.Rect {
    init(_ rec: NSRect) {
        self = Gfx.Rect(x: Int(self.origin.x), y: Int(self.origin.y), width: Int(self.width), height: Int(self.height))
    }
    func toNSRect(_ gfxR: Gfx.Rect) -> NSRect {
        return NSMakeRect(CGFloat(gfxR.x), CGFloat(gfxR.y), CGFloat(gfxR.width), CGFloat(gfxR.height))
    }
}

extension NSRect {
    init(_ gfxR: Gfx.Rect) {
        self = NSMakeRect(CGFloat(gfxR.x), CGFloat(gfxR.y), CGFloat(gfxR.width), CGFloat(gfxR.height))
    }
}
