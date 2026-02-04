import AppKit
import SwiftUI
import AVFoundation

@MainActor
final class OpenFlowApp: NSObject, NSApplicationDelegate {
    private var statusItem: NSStatusItem?
    private var bubbleWindow: BubbleWindow?
    private let historyStore = HistoryStore()
    private let configStore = ConfigStore()
    private let bubbleState = BubbleState()
    private let hotkeyMonitor = HotkeyMonitor()
    private let audioRecorder = AudioRecorder()
    private let transcriptionRunner = TranscriptionRunner()
    private var levelTimer: Timer?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)

        if let addText = ArgumentParser.value(after: "--add") {
            historyStore.append(text: addText)
            Clipboard.copy(addText)
            NSApp.terminate(nil)
            return
        }

        requestAccessibilityIfNeeded()
        requestMicrophoneIfNeeded()

        configStore.load()
        transcriptionRunner.dictionaryPath = configStore.config.dictionaryPath
        historyStore.load()
        setupStatusItem()
        setupHotkey()
        showBubble()
    }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.title = "OF"

        let menu = NSMenu()
        let toggleBubbleItem = NSMenuItem(title: "Toggle Bubble", action: #selector(toggleBubble), keyEquivalent: "b")
        toggleBubbleItem.target = self
        menu.addItem(toggleBubbleItem)

        let reloadHistoryItem = NSMenuItem(title: "Reload History", action: #selector(reloadHistory), keyEquivalent: "r")
        reloadHistoryItem.target = self
        menu.addItem(reloadHistoryItem)

        menu.addItem(NSMenuItem.separator())

        let historyMenu = NSMenu(title: "History")
        let historyItem = NSMenuItem(title: "History", action: nil, keyEquivalent: "")
        historyItem.submenu = historyMenu
        menu.addItem(historyItem)
        rebuildHistoryMenu(historyMenu)

        menu.addItem(NSMenuItem.separator())

        let quitItem = NSMenuItem(title: "Quit", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        item.menu = menu
        statusItem = item
    }

    private func setupHotkey() {
        hotkeyMonitor.onFnDown = { [weak self] in
            self?.startListening()
        }
        hotkeyMonitor.onFnUp = { [weak self] in
            self?.stopListening()
        }
        hotkeyMonitor.start()
    }

    private func startListening() {
        guard !bubbleState.isListening else { return }
        bubbleState.isListening = true
        do {
            _ = try audioRecorder.start()
            startLevelMeter()
        } catch {
            bubbleState.isListening = false
            return
        }
    }

    private func stopListening() {
        guard bubbleState.isListening else { return }
        bubbleState.isListening = false
        stopLevelMeter()
        guard let url = audioRecorder.stop() else { return }

        transcriptionRunner.transcribe(audioURL: url) { [weak self] text in
            guard let self else { return }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !trimmed.isEmpty else { return }
            self.historyStore.append(text: trimmed)
            self.refreshHistoryMenu()
            AccessibilityPaster.paste(trimmed)
        }
    }

    private func refreshHistoryMenu() {
        if let menu = statusItem?.menu?.item(withTitle: "History")?.submenu {
            rebuildHistoryMenu(menu)
        }
    }

    private func rebuildHistoryMenu(_ menu: NSMenu) {
        menu.removeAllItems()
        let items = historyStore.entries.reversed()
        if items.isEmpty {
            let empty = NSMenuItem(title: "No history yet", action: nil, keyEquivalent: "")
            empty.isEnabled = false
            menu.addItem(empty)
            return
        }

        for entry in items.prefix(25) {
            let title = entry.text.trimmingCharacters(in: .whitespacesAndNewlines)
            let display = title.count > 60 ? String(title.prefix(57)) + "..." : title
            let item = NSMenuItem(title: display, action: #selector(copyHistoryItem(_:)), keyEquivalent: "")
            item.target = self
            item.representedObject = entry
            menu.addItem(item)
        }
    }

    private func showBubble() {
        if bubbleWindow == nil {
            let view = BubbleView().environmentObject(bubbleState)
            let hosting = NSHostingView(rootView: view)
            let panel = BubbleWindow(content: hosting, size: NSSize(width: 160, height: 32))
            panel.positionBottomCenter()
            bubbleWindow = panel
        }
        bubbleWindow?.orderFrontRegardless()
    }

    private func hideBubble() {
        bubbleWindow?.orderOut(nil)
    }

    @objc private func toggleBubble() {
        guard let window = bubbleWindow else {
            showBubble()
            return
        }
        if window.isVisible {
            hideBubble()
        } else {
            showBubble()
        }
    }

    @objc private func reloadHistory() {
        historyStore.load()
        refreshHistoryMenu()
    }

    @objc private func copyHistoryItem(_ sender: NSMenuItem) {
        guard let entry = sender.representedObject as? HistoryEntry else { return }
        Clipboard.copy(entry.text)
    }

    @objc private func quit() {
        hotkeyMonitor.stop()
        NSApp.terminate(nil)
    }

    private func startLevelMeter() {
        levelTimer?.invalidate()
        levelTimer = Timer.scheduledTimer(withTimeInterval: 0.05, repeats: true) { [weak self] _ in
            guard let self else { return }
            Task { @MainActor in
                bubbleState.level = audioRecorder.currentLevel()
            }
        }
    }

    private func stopLevelMeter() {
        levelTimer?.invalidate()
        levelTimer = nil
        bubbleState.level = 0
    }

    private func requestAccessibilityIfNeeded() {
        let key = "AXTrustedCheckOptionPrompt" as CFString
        let options = [key: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
    }

    private func requestMicrophoneIfNeeded() {
        AVCaptureDevice.requestAccess(for: .audio) { _ in }
    }
}

@main
struct OpenFlowMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = OpenFlowApp()
        app.delegate = delegate
        app.run()
    }
}

final class BubbleWindow: NSPanel {
    init(content: NSView, size: NSSize) {
        super.init(
            contentRect: NSRect(origin: .zero, size: size),
            styleMask: [.borderless, .nonactivatingPanel],
            backing: .buffered,
            defer: false
        )
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        level = .statusBar
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        ignoresMouseEvents = true
        contentView = content
    }

    func positionBottomCenter() {
        guard let screen = NSScreen.main else { return }
        let frame = screen.visibleFrame
        let origin = NSPoint(x: frame.midX - (self.frame.width / 2), y: frame.minY + 20)
        setFrameOrigin(origin)
    }

    func setBubbleSize(isListening: Bool) {
        positionBottomCenter()
    }
}

final class BubbleState: ObservableObject {
    @Published var isListening = false
    @Published var level: Double = 0
}

struct BubbleView: View {
    @EnvironmentObject var state: BubbleState

    var body: some View {
        ZStack {
            if state.isListening {
                HStack(spacing: 8) {
                    Circle()
                        .fill(Color.red)
                        .frame(width: 8, height: 8)
                    Text("Listening")
                        .font(.system(size: 12, weight: .medium))
                    LevelBarGraph(level: state.level)
                        .frame(width: 42, height: 10)
                }
                .padding(.horizontal, 12)
            }
        }
        .frame(width: 160, height: 32)
        .background(
            RoundedRectangle(cornerRadius: state.isListening ? 16 : 3, style: .continuous)
                .fill(Color.black.opacity(state.isListening ? 0.85 : 0.35))
                .frame(width: state.isListening ? 160 : 24, height: state.isListening ? 32 : 6)
        )
        .animation(.spring(response: 0.2, dampingFraction: 0.9), value: state.isListening)
        .foregroundStyle(.white)
    }
}

final class HotkeyMonitor {
    var onFnDown: (() -> Void)?
    var onFnUp: (() -> Void)?

    private var globalMonitor: Any?
    private var localMonitor: Any?
    private var fnIsDown = false

    func start() {
        localMonitor = NSEvent.addLocalMonitorForEvents(matching: .flagsChanged) { [weak self] event in
            self?.handle(event)
            return event
        }
        globalMonitor = NSEvent.addGlobalMonitorForEvents(matching: .flagsChanged) { [weak self] event in
            self?.handle(event)
        }
    }

    func stop() {
        if let monitor = globalMonitor {
            NSEvent.removeMonitor(monitor)
        }
        if let monitor = localMonitor {
            NSEvent.removeMonitor(monitor)
        }
        globalMonitor = nil
        localMonitor = nil
    }

    private func handle(_ event: NSEvent) {
        guard event.keyCode == 63 else { return }
        let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
        let isDown = flags.contains(.function)

        if isDown && !fnIsDown {
            fnIsDown = true
            onFnDown?()
        } else if !isDown && fnIsDown {
            fnIsDown = false
            onFnUp?()
        }
    }
}

final class AudioRecorder: NSObject, AVAudioRecorderDelegate {
    private var recorder: AVAudioRecorder?
    private var currentURL: URL?

    func start() throws -> URL {
        let url = Paths.recordingURL
        let settings: [String: Any] = [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVSampleRateKey: 16_000,
            AVNumberOfChannelsKey: 1,
            AVLinearPCMBitDepthKey: 16,
            AVLinearPCMIsFloatKey: false,
            AVLinearPCMIsBigEndianKey: false
        ]
        let recorder = try AVAudioRecorder(url: url, settings: settings)
        recorder.delegate = self
        recorder.isMeteringEnabled = true
        recorder.prepareToRecord()
        recorder.record()
        self.recorder = recorder
        self.currentURL = url
        return url
    }

    func stop() -> URL? {
        recorder?.stop()
        let url = currentURL
        recorder = nil
        currentURL = nil
        return url
    }

    func currentLevel() -> Double {
        guard let recorder else { return 0 }
        recorder.updateMeters()
        let db = Double(recorder.averagePower(forChannel: 0))
        let linear = pow(10.0, db / 20.0)
        return min(1.0, max(0.0, linear))
    }
}

struct LevelBarGraph: View {
    let level: Double
    private let scales: [Double] = [0.4, 0.55, 0.7, 0.9, 1.0, 0.8, 0.6, 0.75, 1.0, 0.7, 0.5, 0.4]

    var body: some View {
        HStack(spacing: 2) {
            ForEach(0..<scales.count, id: \.self) { idx in
                let height = max(2, level * scales[idx] * 10)
                RoundedRectangle(cornerRadius: 1.5, style: .continuous)
                    .fill(Color.white.opacity(0.9))
                    .frame(width: 2, height: height)
                    .frame(height: 10, alignment: .bottom)
            }
        }
    }
}

final class TranscriptionRunner {
    private let queue = DispatchQueue(label: "openflow.transcription", qos: .userInitiated)
    var dictionaryPath: String?

    func transcribe(audioURL: URL, completion: @escaping (String) -> Void) {
        queue.async {
            let result = self.runVadTranscriber(audioURL: audioURL)
            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

    private func runVadTranscriber(audioURL: URL) -> String {
        guard let vadPath = Paths.vadTranscriberURL,
              let modelPath = Paths.whisperModelURL,
              let sileroPath = Paths.sileroModelURL else {
            return ""
        }

        let process = Process()
        process.executableURL = vadPath
        var args = [
            "--audio-file", audioURL.path,
            "--silero-vad", sileroPath.path,
            "--model", modelPath.path
        ]
        if let dictionaryURL = Paths.dictionaryURL(overridePath: dictionaryPath),
           FileManager.default.fileExists(atPath: dictionaryURL.path) {
            args += ["--dictionary-file", dictionaryURL.path]
        }
        process.arguments = args

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = nil

        do {
            try process.run()
        } catch {
            return ""
        }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return TranscriptionRunner.extractTranscript(from: data)
    }

    private static func extractTranscript(from data: Data) -> String {
        guard let output = String(data: data, encoding: .utf8) else { return "" }
        var segments: [String] = []

        for line in output.split(separator: "\n") {
            guard line.contains("\"event\":\"segment\"") else { continue }
            guard let jsonData = line.data(using: .utf8) else { continue }
            guard let obj = try? JSONSerialization.jsonObject(with: jsonData) as? [String: Any] else { continue }
            guard let text = obj["text"] as? String else { continue }
            if let finalFlag = obj["final"] as? Bool, finalFlag == false {
                continue
            }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                segments.append(trimmed)
            }
        }

        return segments.joined(separator: " ")
    }
}

struct HistoryEntry: Codable, Hashable {
    let id: UUID
    let timestamp: Date
    let text: String
}

final class HistoryStore {
    private(set) var entries: [HistoryEntry] = []

    func load() {
        entries = []
        guard let url = Paths.historyURL else { return }
        guard let data = try? Data(contentsOf: url) else { return }
        guard let text = String(data: data, encoding: .utf8) else { return }
        for line in text.split(separator: "\n") {
            guard let lineData = line.data(using: .utf8) else { continue }
            if let entry = try? JSONDecoder().decode(HistoryEntry.self, from: lineData) {
                entries.append(entry)
            }
        }
    }

    func append(text: String) {
        guard let url = Paths.historyURL else { return }
        Paths.ensureConfigDir()
        let entry = HistoryEntry(id: UUID(), timestamp: Date(), text: text)
        guard let data = try? JSONEncoder().encode(entry) else { return }
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(data)
            handle.write(Data("\n".utf8))
            try? handle.close()
        } else {
            try? data.write(to: url)
            if let handle = try? FileHandle(forWritingTo: url) {
                handle.seekToEndOfFile()
                handle.write(Data("\n".utf8))
                try? handle.close()
            }
        }
        entries.append(entry)
    }
}

struct Config: Codable {
    var apiKey: String?
    var dictionaryPath: String?
}

final class ConfigStore {
    private(set) var config = Config()

    func load() {
        guard let url = Paths.configURL else { return }
        guard let data = try? Data(contentsOf: url) else { return }
        if let loaded = try? JSONDecoder().decode(Config.self, from: data) {
            config = loaded
        }
    }
}

enum Clipboard {
    static func copy(_ text: String) {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
    }
}

enum AccessibilityPaster {
    static func paste(_ text: String) {
        Clipboard.copy(text)
        let source = CGEventSource(stateID: .combinedSessionState)
        let cmdDown = CGEvent(keyboardEventSource: source, virtualKey: KeyCodes.command, keyDown: true)
        let vDown = CGEvent(keyboardEventSource: source, virtualKey: KeyCodes.v, keyDown: true)
        vDown?.flags = .maskCommand
        let vUp = CGEvent(keyboardEventSource: source, virtualKey: KeyCodes.v, keyDown: false)
        vUp?.flags = .maskCommand
        let cmdUp = CGEvent(keyboardEventSource: source, virtualKey: KeyCodes.command, keyDown: false)

        cmdDown?.post(tap: .cghidEventTap)
        vDown?.post(tap: .cghidEventTap)
        vUp?.post(tap: .cghidEventTap)
        cmdUp?.post(tap: .cghidEventTap)
    }
}

enum KeyCodes {
    static let command: CGKeyCode = 55
    static let v: CGKeyCode = 9
}

enum Paths {
    static var configDirURL: URL? {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".config", isDirectory: true)
            .appendingPathComponent("openflow", isDirectory: true)
    }

    static var configURL: URL? {
        configDirURL?.appendingPathComponent("config.json")
    }

    static var historyURL: URL? {
        configDirURL?.appendingPathComponent("history.jsonl")
    }

    static func dictionaryURL(overridePath: String?) -> URL? {
        if let overridePath, !overridePath.isEmpty {
            return URL(fileURLWithPath: overridePath)
        }
        return configDirURL?.appendingPathComponent("dictionary.txt")
    }

    static var recordingURL: URL {
        let dir = FileManager.default.temporaryDirectory
        return dir.appendingPathComponent("openflow_recording.wav")
    }

    static func ensureConfigDir() {
        guard let url = configDirURL else { return }
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    }

    static var repoRootURL: URL? {
        let start = Bundle.main.executableURL ?? URL(fileURLWithPath: CommandLine.arguments[0])
        var cursor = start.deletingLastPathComponent()
        for _ in 0..<10 {
            let candidate = cursor.appendingPathComponent("transcriber", isDirectory: true)
            if FileManager.default.fileExists(atPath: candidate.path) {
                return cursor
            }
            let next = cursor.deletingLastPathComponent()
            if next.path == cursor.path { break }
            cursor = next
        }
        return nil
    }

    static var clientDirURL: URL? {
        repoRootURL?.appendingPathComponent("transcriber", isDirectory: true)
    }

    static var vadTranscriberURL: URL? {
        clientDirURL?.appendingPathComponent("build/bin/vad_transcriber")
    }

    static var whisperModelURL: URL? {
        clientDirURL?.appendingPathComponent("whisper.cpp/models/ggml-base.en.bin")
    }

    static var sileroModelURL: URL? {
        clientDirURL?.appendingPathComponent("whisper.cpp/models/ggml-silero-v5.1.2.bin")
    }
}

enum ArgumentParser {
    static func value(after flag: String) -> String? {
        let args = CommandLine.arguments
        guard let idx = args.firstIndex(of: flag), idx + 1 < args.count else { return nil }
        return args[idx + 1]
    }
}
