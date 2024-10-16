import Foundation
@_exported import GfxCxx

extension Gfx.Size {
	init(_ size: NSSize) {
		self = Gfx.Size(width: Int(self.width), height: Int(self.height))
	}
}

extension NSSize {
	init(_ gfxS: Gfx.Size) {
		self = NSMakeSize(CGFloat(gfxS.width), CGFloat(gfxS.height))
	}
}
