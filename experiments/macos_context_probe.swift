#!/usr/bin/env swift
import Foundation
import AppKit
import Carbon.HIToolbox

func now() -> String {
    let f = ISO8601DateFormatter()
    f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return f.string(from: Date())
}

func dumpOnScreenWindows() {
    guard
        let raw = CGWindowListCopyWindowInfo([.optionOnScreenOnly, .excludeDesktopElements], kCGNullWindowID)
            as? [[String: Any]]
    else {
        print("[\(now())] failed to read on-screen windows")
        return
    }

    print("[\(now())] on-screen windows (\(raw.count))")
    for window in raw.prefix(50) {
        let owner = (window[kCGWindowOwnerName as String] as? String) ?? "<unknown-app>"
        let title = (window[kCGWindowName as String] as? String) ?? ""
        let id = (window[kCGWindowNumber as String] as? Int) ?? -1
        let layer = (window[kCGWindowLayer as String] as? Int) ?? -1
        print("  id=\(id) layer=\(layer) app=\(owner) title=\(title)")
    }
}

func getChromeTabs() -> [(String, String, String, String)] {
    let script = """
    tell application \"Google Chrome\"
        if it is not running then return \"\"
        set output to \"\"
        set winCount to count of windows
        repeat with wIndex from 1 to winCount
            set tabCount to count of tabs of window wIndex
            repeat with tIndex from 1 to tabCount
                try
                    set tTitle to title of tab tIndex of window wIndex
                on error
                    set tTitle to \"\"
                end try
                try
                    set tURL to URL of tab tIndex of window wIndex
                on error
                    set tURL to \"\"
                end try
                set output to output & (wIndex as string) & tab & (tIndex as string) & tab & tTitle & tab & tURL & linefeed
            end repeat
        end repeat
        return output
    end tell
    """

    guard let appleScript = NSAppleScript(source: script) else { return [] }

    var errorDict: NSDictionary?
    let result = appleScript.executeAndReturnError(&errorDict)
    if let errorDict {
        let message = (errorDict[NSAppleScript.errorMessage] as? String) ?? "unknown error"
        print("[\(now())] chrome query error: \(message)")
        return []
    }

    let text = result.stringValue ?? ""
    if text.isEmpty { return [] }

    let rows = text.split(separator: "\n")
    return rows.compactMap { row in
        let cols = row.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        guard cols.count >= 4 else { return nil }
        return (cols[0], cols[1], cols[2], cols[3])
    }
}

func dumpChromeTabs() {
    let tabs = getChromeTabs()
    print("[\(now())] chrome tabs (\(tabs.count))")
    for (windowIdx, tabIdx, title, url) in tabs.prefix(200) {
        print("  window=\(windowIdx) tab=\(tabIdx) title=\(title) url=\(url)")
    }
}

// --- Global keystroke monitor via CGEventTap (requires Accessibility permission) ---

// Buffer to accumulate "typed into the void" keystrokes
var keystrokeBuffer = ""
var lastKeystrokeTime = Date()
let bufferTimeout: TimeInterval = 1.5  // flush after 1.5s of silence

func checkAccessibilityPermission() -> Bool {
    let trusted = AXIsProcessTrustedWithOptions(
        [kAXTrustedCheckOptionPrompt.takeUnretainedValue(): true] as CFDictionary
    )
    return trusted
}

func keyCodeToString(_ keyCode: UInt16, _ event: CGEvent) -> String? {
    // Use the event's unicode string which respects keyboard layout and modifiers
    let maxLen = 4
    var actualLen = 0
    var chars = [UniChar](repeating: 0, count: maxLen)
    event.keyboardGetUnicodeString(maxStringLength: maxLen, actualStringLength: &actualLen, unicodeString: &chars)
    if actualLen > 0 {
        return String(utf16CodeUnits: chars, count: actualLen)
    }
    return nil
}

func flushKeystrokeBuffer() {
    guard !keystrokeBuffer.isEmpty else { return }
    let query = keystrokeBuffer.trimmingCharacters(in: .whitespacesAndNewlines)
    if !query.isEmpty {
        print("[\(now())] QUERY BUFFER: \"\(query)\"")
    }
    keystrokeBuffer = ""
}

func installGlobalKeystrokeMonitor() {
    guard checkAccessibilityPermission() else {
        print("[\(now())] Accessibility permission NOT granted. Go to:")
        print("  System Settings > Privacy & Security > Accessibility")
        print("  and add Terminal (or whatever is running this script).")
        print("  Then re-run. Keystroke capture will be skipped.")
        return
    }
    print("[\(now())] Accessibility permission granted, installing global key monitor...")

    let eventMask: CGEventMask = (1 << CGEventType.keyDown.rawValue)

    guard let eventTap = CGEvent.tapCreate(
        tap: .cgSessionEventTap,
        place: .headInsertEventTap,
        options: .listenOnly,       // passive – doesn't block or modify events
        eventsOfInterest: eventMask,
        callback: { (proxy, type, event, refcon) -> Unmanaged<CGEvent>? in
            if type == .keyDown {
                let keyCode = UInt16(event.getIntegerValueField(.keyboardEventKeycode))
                let flags = event.flags

                // Detect modifier-only combos (cmd, ctrl, etc.) – skip those
                let modifierOnly = flags.contains(.maskCommand) || flags.contains(.maskControl)

                if let char = keyCodeToString(keyCode, event), !modifierOnly {
                    let frontApp = NSWorkspace.shared.frontmostApplication?.localizedName ?? "?"
                    print("[\(now())] KEY: code=\(keyCode) char=\(char) app=\(frontApp)")

                    // Accumulate into buffer
                    keystrokeBuffer += char
                    lastKeystrokeTime = Date()
                } else if keyCode == UInt16(kVK_Return) || keyCode == UInt16(kVK_Escape) {
                    flushKeystrokeBuffer()
                } else if keyCode == UInt16(kVK_Delete) {
                    // Backspace – remove last char from buffer
                    if !keystrokeBuffer.isEmpty {
                        keystrokeBuffer.removeLast()
                    }
                }
            }
            return Unmanaged.passRetained(event)
        },
        userInfo: nil
    ) else {
        print("[\(now())] Failed to create CGEventTap. Check accessibility permissions.")
        return
    }

    let runLoopSource = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap, 0)
    CFRunLoopAddSource(CFRunLoopGetMain(), runLoopSource, .commonModes)
    CGEvent.tapEnable(tap: eventTap, enable: true)
    print("[\(now())] Global keystroke monitor installed (listen-only, passive)")

    // Timer to flush the buffer after inactivity
    Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { _ in
        if !keystrokeBuffer.isEmpty && Date().timeIntervalSince(lastKeystrokeTime) > bufferTimeout {
            flushKeystrokeBuffer()
        }
    }
}

func installAppSwitchMonitor() {
    NSWorkspace.shared.notificationCenter.addObserver(
        forName: NSWorkspace.didActivateApplicationNotification,
        object: nil,
        queue: .main
    ) { note in
        guard
            let app = note.userInfo?[NSWorkspace.applicationUserInfoKey] as? NSRunningApplication,
            let name = app.localizedName
        else {
            return
        }

        print("[\(now())] app switch -> \(name) (pid=\(app.processIdentifier))")

        if name == "Google Chrome" {
            dumpChromeTabs()
        }
    }
}

print("Starting macOS context probe")
print("This script captures global keystrokes via Accessibility API (CGEventTap).")
print("To query Chrome tabs, allow Automation permission when prompted.")

// Initial snapshot
dumpOnScreenWindows()
dumpChromeTabs()
installAppSwitchMonitor()
installGlobalKeystrokeMonitor()

// Periodic snapshots (windows + chrome tabs)
Timer.scheduledTimer(withTimeInterval: 15.0, repeats: true) { _ in
    dumpOnScreenWindows()
    dumpChromeTabs()
}

RunLoop.main.run()
