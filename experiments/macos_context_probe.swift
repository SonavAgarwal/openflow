#!/usr/bin/env swift
import Foundation
import AppKit
import Carbon.HIToolbox
import ApplicationServices

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

func getActiveChromeTab() -> (String, String, String, String)? {
    let script = """
    tell application \"Google Chrome\"
        if it is not running then return \"\"
        if (count of windows) is 0 then return \"\"
        set wIndex to index of front window
        set tIndex to active tab index of front window
        set tTitle to title of active tab of front window
        set tURL to URL of active tab of front window
        return (wIndex as string) & tab & (tIndex as string) & tab & tTitle & tab & tURL
    end tell
    """

    guard let appleScript = NSAppleScript(source: script) else { return nil }

    var errorDict: NSDictionary?
    let result = appleScript.executeAndReturnError(&errorDict)
    if let errorDict {
        let message = (errorDict[NSAppleScript.errorMessage] as? String) ?? "unknown error"
        print("[\(now())] chrome active-tab query error: \(message)")
        return nil
    }

    let text = result.stringValue ?? ""
    if text.isEmpty { return nil }

    let cols = text.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
    guard cols.count >= 4 else { return nil }
    return (cols[0], cols[1], cols[2], cols[3])
}

func dumpActiveChromeTab() {
    guard let (windowIdx, tabIdx, title, url) = getActiveChromeTab() else {
        print("[\(now())] chrome active tab unavailable")
        return
    }

    print("[\(now())] chrome active tab window=\(windowIdx) tab=\(tabIdx) title=\(title) url=\(url)")
}

func copyAXAttribute(_ element: AXUIElement, _ attribute: String) -> CFTypeRef? {
    var value: CFTypeRef?
    let error = AXUIElementCopyAttributeValue(element, attribute as CFString, &value)
    guard error == .success else { return nil }
    return value
}

func copyAXStringAttribute(_ element: AXUIElement, _ attribute: String) -> String? {
    guard let value = copyAXAttribute(element, attribute) else { return nil }

    if let string = value as? String {
        return string
    }

    if let attributedString = value as? NSAttributedString {
        return attributedString.string
    }

    return nil
}

func isAXUIElement(_ value: CFTypeRef) -> Bool {
    CFGetTypeID(value) == AXUIElementGetTypeID()
}

func copyAXAttributeNames(_ element: AXUIElement) -> [String] {
    var names: CFArray?
    let error = AXUIElementCopyAttributeNames(element, &names)
    guard error == .success, let names = names as? [String] else { return [] }
    return names
}

func copyAXElementAttribute(_ element: AXUIElement, _ attribute: String) -> AXUIElement? {
    guard let value = copyAXAttribute(element, attribute), isAXUIElement(value) else { return nil }
    return (value as! AXUIElement)
}

func copyAXElementArray(_ value: CFTypeRef) -> [AXUIElement] {
    if isAXUIElement(value) {
        return [(value as! AXUIElement)]
    }

    guard let children = value as? [Any], !children.isEmpty else { return [] }
    return children.compactMap { child -> AXUIElement? in
        let childRef = child as CFTypeRef
        guard isAXUIElement(childRef) else { return nil }
        return (childRef as! AXUIElement)
    }
}

func axNodeID(_ element: AXUIElement) -> Int {
    Int(CFHash(element))
}

func isDescendingAXAttribute(_ attribute: String) -> Bool {
    let excluded: Set<String> = [
        kAXParentAttribute as String,
        kAXWindowAttribute as String,
        kAXTopLevelUIElementAttribute as String,
        kAXFocusedWindowAttribute as String,
        kAXFocusedUIElementAttribute as String
    ]
    if excluded.contains(attribute) {
        return false
    }

    let preferred: Set<String> = [
        kAXChildrenAttribute as String,
        kAXVisibleChildrenAttribute as String,
        kAXContentsAttribute as String,
        kAXRowsAttribute as String,
        kAXColumnsAttribute as String,
        kAXTabsAttribute as String,
        kAXTitleUIElementAttribute as String,
        kAXServesAsTitleForUIElementsAttribute as String,
        kAXLinkedUIElementsAttribute as String
    ]
    if preferred.contains(attribute) {
        return true
    }

    return
        attribute.hasSuffix("Children")
        || attribute.hasSuffix("Elements")
        || attribute.hasSuffix("UIElement")
        || attribute.hasSuffix("UIElements")
        || attribute.hasSuffix("Contents")
}

func copyAXDescendants(_ element: AXUIElement) -> [AXUIElement] {
    var descendants: [AXUIElement] = []
    var seen = Set<Int>()

    for attribute in copyAXAttributeNames(element) where isDescendingAXAttribute(attribute) {
        guard let value = copyAXAttribute(element, attribute) else { continue }
        for child in copyAXElementArray(value) {
            let id = axNodeID(child)
            guard !seen.contains(id) else { continue }
            seen.insert(id)
            descendants.append(child)
        }
    }

    return descendants
}

func copyAXRole(_ element: AXUIElement) -> String {
    copyAXStringAttribute(element, kAXRoleAttribute as String) ?? ""
}

func copyAXSubrole(_ element: AXUIElement) -> String {
    copyAXStringAttribute(element, kAXSubroleAttribute as String) ?? ""
}

func summarizeAXNode(_ element: AXUIElement) -> String {
    let role = copyAXRole(element)
    let subrole = copyAXSubrole(element)
    let title = normalizeAXText(copyAXStringAttribute(element, kAXTitleAttribute as String) ?? "")

    var parts = [role]
    if !subrole.isEmpty {
        parts.append(subrole)
    }
    if !title.isEmpty {
        parts.append("title=\(title)")
    }
    return parts.joined(separator: " ")
}

func normalizeAXText(_ text: String) -> String {
    text.replacingOccurrences(of: "\\s+", with: " ", options: .regularExpression)
        .trimmingCharacters(in: .whitespacesAndNewlines)
}

func appendAXText(_ text: String?, lines: inout [String], seen: inout Set<String>) {
    guard let text else { return }
    let normalized = normalizeAXText(text)
    guard !normalized.isEmpty else { return }
    guard normalized.count <= 500 else { return }
    guard !seen.contains(normalized) else { return }

    seen.insert(normalized)
    lines.append(normalized)
}

func collectAXTextForElement(_ element: AXUIElement, lines: inout [String], seenLines: inout Set<String>) {
    appendAXText(copyAXStringAttribute(element, kAXTitleAttribute as String), lines: &lines, seen: &seenLines)
    appendAXText(copyAXStringAttribute(element, kAXValueAttribute as String), lines: &lines, seen: &seenLines)
    appendAXText(copyAXStringAttribute(element, kAXDescriptionAttribute as String), lines: &lines, seen: &seenLines)
    appendAXText(copyAXStringAttribute(element, kAXHelpAttribute as String), lines: &lines, seen: &seenLines)
    appendAXText(copyAXStringAttribute(element, kAXSelectedTextAttribute as String), lines: &lines, seen: &seenLines)
    appendAXText(copyAXStringAttribute(element, "AXPlaceholderValue"), lines: &lines, seen: &seenLines)
    appendAXText(copyAXStringAttribute(element, "AXRoleDescription"), lines: &lines, seen: &seenLines)
}

func collectAXTree(
    roots: [AXUIElement],
    maxDepth: Int,
    maxNodes: Int,
    lines: inout [String],
    seenLines: inout Set<String>,
    webAreas: inout [AXUIElement],
    roleCounts: inout [String: Int]
) -> Int {
    var stack = roots.reversed().map { ($0, 0) }
    var seenNodes = Set<Int>()
    var visitedNodes = 0

    while let (element, depth) = stack.popLast() {
        guard depth <= maxDepth else { continue }
        let id = axNodeID(element)
        guard !seenNodes.contains(id) else { continue }
        seenNodes.insert(id)

        visitedNodes += 1
        if visitedNodes > maxNodes {
            break
        }

        let role = copyAXRole(element)
        roleCounts[role, default: 0] += 1
        if role == "AXWebArea" {
            webAreas.append(element)
        }

        collectAXTextForElement(element, lines: &lines, seenLines: &seenLines)

        for child in copyAXDescendants(element).reversed() {
            stack.append((child, depth + 1))
        }
    }

    return visitedNodes
}

func dumpFrontmostAccessibleText() {
    guard checkAccessibilityPermission() else {
        print("[\(now())] accessible text unavailable without Accessibility permission")
        return
    }

    guard let app = NSWorkspace.shared.frontmostApplication else {
        print("[\(now())] frontmost app unavailable")
        return
    }

    let appName = app.localizedName ?? "?"
    let appElement = AXUIElementCreateApplication(app.processIdentifier)
    let focusedWindow = copyAXElementAttribute(appElement, kAXFocusedWindowAttribute as String)
    let focusedElement = copyAXElementAttribute(appElement, kAXFocusedUIElementAttribute as String)

    var initialRoots: [AXUIElement] = []
    for candidate in [focusedWindow, focusedElement, appElement] {
        guard let candidate else { continue }
        let id = axNodeID(candidate)
        if !initialRoots.contains(where: { axNodeID($0) == id }) {
            initialRoots.append(candidate)
        }
    }

    var seenLines = Set<String>()
    var lines: [String] = []
    var roleCounts: [String: Int] = [:]
    var discoveredWebAreas: [AXUIElement] = []

    let initialVisited = collectAXTree(
        roots: initialRoots,
        maxDepth: 14,
        maxNodes: 2000,
        lines: &lines,
        seenLines: &seenLines,
        webAreas: &discoveredWebAreas,
        roleCounts: &roleCounts
    )

    let webAreaRoots =
        Array(Dictionary(grouping: discoveredWebAreas, by: { axNodeID($0) }).values.compactMap(\.first))

    var deepLines = lines
    var deepSeenLines = seenLines
    var deepRoleCounts = roleCounts
    var nestedWebAreas: [AXUIElement] = []
    var totalVisited = initialVisited

    if !webAreaRoots.isEmpty {
        totalVisited += collectAXTree(
            roots: webAreaRoots,
            maxDepth: 20,
            maxNodes: 5000,
            lines: &deepLines,
            seenLines: &deepSeenLines,
            webAreas: &nestedWebAreas,
            roleCounts: &deepRoleCounts
        )
    }

    let topRoles = deepRoleCounts
        .filter { !$0.key.isEmpty }
        .sorted { lhs, rhs in
            if lhs.value == rhs.value {
                return lhs.key < rhs.key
            }
            return lhs.value > rhs.value
        }
        .prefix(8)
        .map { "\($0.key)=\($0.value)" }
        .joined(separator: ", ")

    let rootsSummary = initialRoots.prefix(3).map(summarizeAXNode).joined(separator: " | ")
    print("[\(now())] accessible text app=\(appName) lines=\(deepLines.count) visited=\(totalVisited) webAreas=\(webAreaRoots.count) roots=\(rootsSummary)")
    if !topRoles.isEmpty {
        print("  roles=\(topRoles)")
    }
    for line in deepLines.prefix(160) {
        print("  text=\(line)")
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
            dumpActiveChromeTab()
            dumpChromeTabs()
        }

        dumpFrontmostAccessibleText()
    }
}

print("Starting macOS context probe")
print("This script captures global keystrokes via Accessibility API (CGEventTap).")
print("To query Chrome tabs, allow Automation permission when prompted.")

// Initial snapshot
dumpOnScreenWindows()
dumpActiveChromeTab()
dumpChromeTabs()
dumpFrontmostAccessibleText()
installAppSwitchMonitor()
installGlobalKeystrokeMonitor()

// Periodic snapshots (windows + chrome tabs)
Timer.scheduledTimer(withTimeInterval: 15.0, repeats: true) { _ in
    dumpOnScreenWindows()
    dumpActiveChromeTab()
    dumpChromeTabs()
    dumpFrontmostAccessibleText()
}

RunLoop.main.run()
