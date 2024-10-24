import Foundation
import AppKit

func isUsingDarkSystemTheme() -> Bool {
    let appearance = NSApp.effectiveAppearance
    let matchedAppearance = appearance.bestMatch(from: [
        .aqua,
        .darkAqua
    ])

    return matchedAppearance == .darkAqua
}

func createSystemPalette() {
    let isDark = isUsingDarkSystemTheme()

    let themeFile = isDark ? "Dark" : "Default"
    let themePath = "resource://themes/\(themeFile).ini"

    guard let themeURL = Bundle.main.url(forResource: themeFile, withExtension: "ini")?.absoluteURL else {
        fatalError("Theme file not found in bundle")
    }


    // FIXME load url into palette and return
}
