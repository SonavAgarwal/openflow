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
    private let llmRefiner = LLMRefiner()
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
        transcriptionRunner.dictionaryText = configStore.config.dictionaryText
        transcriptionRunner.modelName = configStore.config.model
        llmRefiner.apiKey = resolveApiKey()
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
            print("[transcription] heard: \(trimmed)")
            llmRefiner.refine(text: trimmed) { refined in
                let finalText = refined.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? trimmed : refined
                self.historyStore.append(text: finalText)
                self.refreshHistoryMenu()
                AccessibilityPaster.paste(finalText)
            }
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
                let level = audioRecorder.currentLevel()
                bubbleState.level = level
                bubbleState.pushLevel(level)
            }
        }
    }

    private func stopLevelMeter() {
        levelTimer?.invalidate()
        levelTimer = nil
        bubbleState.level = 0
        bubbleState.levelHistory = Array(repeating: 0, count: 12)
    }

    private func requestAccessibilityIfNeeded() {
        let key = "AXTrustedCheckOptionPrompt" as CFString
        let options = [key: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
    }

    private func requestMicrophoneIfNeeded() {
        AVCaptureDevice.requestAccess(for: .audio) { _ in }
    }

    private func resolveApiKey() -> String? {
        if let key = configStore.config.apiKey, !key.isEmpty {
            return key
        }
        if let key = ProcessInfo.processInfo.environment["OPENROUTER_API_KEY"], !key.isEmpty {
            return key
        }
        return nil
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
    @Published var levelHistory: [Double] = Array(repeating: 0, count: 12)

    func pushLevel(_ value: Double) {
        var next = levelHistory
        if next.isEmpty {
            next = Array(repeating: 0, count: 12)
        }
        next.append(value)
        if next.count > 12 {
            next.removeFirst(next.count - 12)
        }
        levelHistory = next
    }
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
                    LevelBarGraph(levels: state.levelHistory)
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
        let db = Double(recorder.peakPower(forChannel: 0))
        if db.isNaN { return 0 }
        // Map -60dB...0dB into 0...1
        let normalized = (db + 60.0) / 60.0
        return min(1.0, max(0.0, normalized))
    }
}

struct LevelBarGraph: View {
    let levels: [Double]

    var body: some View {
        HStack(spacing: 2) {
            ForEach(0..<levels.count, id: \.self) { idx in
                let level = levels[idx]
                let height = max(2, level * 10)
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
    var dictionaryText: [String]?
    var modelName: String?

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
              let modelPath = Paths.whisperModelURL(modelName: modelName),
              let sileroPath = Paths.sileroModelURL else {
            return ""
        }

        let process = Process()
        process.executableURL = vadPath
        var args = [
            "--audio-file", audioURL.path,
            "--silero-vad", sileroPath.path,
            "--model", modelPath.path,
            "--pre-padding-ms", "400",
            "--post-padding-ms", "300"
        ]
        if let dictionaryURL = Paths.dictionaryURL(overridePath: dictionaryPath, dictionaryText: dictionaryText),
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
    var dictionaryText: [String]?
    var model: String?
}

final class ConfigStore {
    private(set) var config = Config()

    func load() {
        guard let url = Paths.configURL else { return }
        guard let data = try? Data(contentsOf: url) else { return }
        if let loaded = try? JSONDecoder().decode(Config.self, from: data) {
            config = loaded
        } else {
            if let raw = String(data: data, encoding: .utf8) {
                print("[config] failed to parse ~/.openflow/config.json")
                print("[config] raw:\n\(raw)")
            } else {
                print("[config] failed to parse ~/.openflow/config.json (unreadable)")
            }
        }
    }

    static func saveApiKey(_ key: String) {
        guard let url = Paths.configURL else { return }
        Paths.ensureConfigDir()
        var config = Config()
        if let data = try? Data(contentsOf: url),
           let loaded = try? JSONDecoder().decode(Config.self, from: data) {
            config = loaded
        }
        config.apiKey = key
        if let data = try? JSONEncoder().encode(config) {
            try? data.write(to: url)
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

final class LLMRefiner {
    var apiKey: String?
    private let client = OpenRouterClient()

    func refine(text: String, completion: @escaping (String) -> Void) {
        guard let apiKey, !apiKey.isEmpty else {
            completion(text)
            return
        }
        client.refine(text: text, apiKey: apiKey) { result in
            completion(result ?? text)
        }
    }
}

final class OpenRouterClient {
    private let endpoint = URL(string: "https://openrouter.ai/api/v1/chat/completions")!

    func refine(text: String, apiKey: String, completion: @escaping (String?) -> Void) {
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
        request.setValue("https://openflow.local", forHTTPHeaderField: "HTTP-Referer")
        request.setValue("OpenFlow", forHTTPHeaderField: "X-Title")

        let systemPrompt = """
You are a transcription refiner. Rewrite the user's dictated text into a clean output.
- Fix punctuation, casing, and spacing. 
- Remove filler words and disfluencies.
- If the user self-corrects (e.g., "6, wait actually 7"), pretend they didn't make their mistake and fix the output accordingly.
- Preserve the user's intent and meaning. Do not add new information.
- Remove cues or tags like "[BLANK_AUDIO]" or *clears throat* or silence or coughing or laughter or other non-verbal sounds.
Return only the refined text.
"""

        let payload: [String: Any] = [
            "model": "openai/gpt-oss-20b",
            "temperature": 0.05,
            "max_tokens": 1000,
            "reasoning": [
                "effort": "high",
                "max_tokens": 2000
            ],
            "provider": [
                "order": ["groq"],
                "allow_fallbacks": false
            ],
            "messages": [
                ["role": "system", "content": systemPrompt],
                ["role": "user", "content": text]
            ]
        ]

        guard let body = try? JSONSerialization.data(withJSONObject: payload) else {
            completion(nil)
            return
        }
        request.httpBody = body

        if let bodyString = String(data: body, encoding: .utf8) {
            print("[openrouter] request: \(bodyString)")
        }

        URLSession.shared.dataTask(with: request) { data, response, _ in
            if let response {
                print("[openrouter] response: \(response)")
            }
            guard let data,
                  let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let choices = obj["choices"] as? [[String: Any]],
                  let message = choices.first?["message"] as? [String: Any],
                  let content = message["content"] as? String else {
                if let data, let raw = String(data: data, encoding: .utf8) {
                    print("[openrouter] response body: \(raw)")
                }
                completion(nil)
                return
            }
            let reasoning = message["reasoning"] as? String
            if let reasoning, !reasoning.isEmpty {
                print("[openrouter] reasoning: \(reasoning)")
            }
            if let usage = obj["usage"] as? [String: Any],
               let completionDetails = usage["completion_tokens_details"] as? [String: Any],
               let reasoningTokens = completionDetails["reasoning_tokens"] {
                print("[openrouter] reasoning_tokens: \(reasoningTokens)")
            }
            print("[openrouter] content: \(content)")
            if let raw = String(data: data, encoding: .utf8) {
                print("[openrouter] response body: \(raw)")
            }
            completion(content)
        }.resume()
    }
}

enum KeyCodes {
    static let command: CGKeyCode = 55
    static let v: CGKeyCode = 9
}

enum Paths {
    static var configDirURL: URL? {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".openflow", isDirectory: true)
    }

    static var configURL: URL? {
        configDirURL?.appendingPathComponent("config.json")
    }

    static var historyURL: URL? {
        configDirURL?.appendingPathComponent("history.jsonl")
    }

    static func dictionaryURL(overridePath: String?, dictionaryText: [String]?) -> URL? {
        if let overridePath, !overridePath.isEmpty {
            return URL(fileURLWithPath: overridePath)
        }
        guard let dir = configDirURL else { return nil }
        let url = dir.appendingPathComponent("dictionary.txt")
        if let dictionaryText, !dictionaryText.isEmpty {
            let joined = dictionaryText
                .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
                .filter { !$0.isEmpty }
                .joined(separator: "\n")
            if !joined.isEmpty {
                ensureConfigDir()
                try? joined.write(to: url, atomically: true, encoding: .utf8)
            }
        }
        return url
    }

    static var recordingURL: URL {
        let dir = FileManager.default.temporaryDirectory
        return dir.appendingPathComponent("openflow_recording.wav")
    }

    static func ensureConfigDir() {
        guard let url = configDirURL else { return }
        try? FileManager.default.createDirectory(at: url, withIntermediateDirectories: true)
    }

    static var bundleResourceURL: URL? {
        Bundle.main.resourceURL
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
        if let bundleResourceURL {
            let candidate = bundleResourceURL.appendingPathComponent("transcriber", isDirectory: true)
            if FileManager.default.fileExists(atPath: candidate.path) {
                return candidate
            }
        }
        return repoRootURL?.appendingPathComponent("transcriber", isDirectory: true)
    }

    static var vadTranscriberURL: URL? {
        clientDirURL?.appendingPathComponent("build/bin/vad_transcriber")
    }

    static func whisperModelURL(modelName: String?) -> URL? {
        let name = (modelName ?? "small").lowercased()
        let file: String
        switch name {
        case "base":
            file = "ggml-base.en.bin"
        case "small":
            file = "ggml-small.en.bin"
        default:
            file = "ggml-small.en.bin"
        }
        return clientDirURL?.appendingPathComponent("whisper.cpp/models/\(file)")
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
