import SwiftUI

@main
struct NeoDroidApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView().preferredColorScheme(.dark)
        }
    }
}

struct SelfTestLine: Identifiable {
    let id = UUID()
    let name: String
    let passed: Bool
    let detail: String
}

struct ContentView: View {
    @State private var lines: [SelfTestLine] = []
    @State private var running = false
    @State private var summary = "P0 自检 / P1 guest 执行"
    @State private var guestReport = ""
    @State private var guestRunning = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    header
                    Divider()

                    Button(action: runSelfTest) {
                        actionLabel(icon: running ? "hourglass" : "play.circle.fill",
                                    title: running ? "Running..." : "Run P0 Self-Test")
                    }
                    .disabled(running)

                    Text(summary).font(.subheadline).foregroundStyle(.secondary)

                    ForEach(lines) { line in
                        HStack(alignment: .top, spacing: 10) {
                            Image(systemName: line.passed ? "checkmark.circle.fill" : "xmark.circle.fill")
                                .foregroundStyle(line.passed ? .green : .red)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(line.name).font(.callout).bold()
                                Text(line.detail).font(.caption).foregroundStyle(.secondary)
                            }
                        }
                        .padding(10)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Color.white.opacity(0.05))
                        .cornerRadius(10)
                    }

                    Divider()

                    Button(action: runGuest) {
                        actionLabel(icon: guestRunning ? "hourglass" : "cpu",
                                    title: guestRunning ? "Executing guest..." : "Run Guest ELF (P1)")
                    }
                    .disabled(guestRunning)

                    if !guestReport.isEmpty {
                        Text(guestReport)
                            .font(.system(.caption, design: .monospaced))
                            .textSelection(.enabled)
                            .padding(10)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color.black.opacity(0.4))
                            .cornerRadius(10)
                    }
                }
                .padding()
            }
            .navigationTitle("NeoDroid")
            .navigationBarTitleDisplayMode(.inline)
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("NeoDroid P1").font(.title2).bold()
            Text("iOS ARM64 Android 运行时 · 原生执行 + syscall ABI 边界")
                .font(.caption).foregroundStyle(.secondary)
            Text(NeoDroidArchitectureInfo())
                .font(.caption2).foregroundStyle(.tertiary).textSelection(.enabled)
        }
    }

    private func actionLabel(icon: String, title: String) -> some View {
        HStack {
            Image(systemName: icon)
            Text(title).fontWeight(.semibold)
        }
        .frame(maxWidth: .infinity)
        .padding()
        .background(Color.accentColor.opacity(0.2))
        .cornerRadius(12)
    }

    private func runSelfTest() {
        running = true
        summary = "运行中..."
        DispatchQueue.global(qos: .userInitiated).async {
            let raw = NeoDroidRunSelfTest()
            let parsed = parse(raw)
            DispatchQueue.main.async {
                lines = parsed.0
                summary = parsed.1
                running = false
            }
        }
    }

    private func runGuest() {
        guestRunning = true
        guestReport = "执行中..."
        DispatchQueue.global(qos: .userInitiated).async {
            let report = NeoDroidRunGuestElf()
            DispatchQueue.main.async {
                guestReport = report
                guestRunning = false
            }
        }
    }

    private func parse(_ raw: String) -> ([SelfTestLine], String) {
        var output: [SelfTestLine] = []
        var ok = 0, total = 0
        for line in raw.split(separator: "\n") {
            let text = String(line)
            if text.hasPrefix("PASS|") {
                let f = text.split(separator: "|", maxSplits: 2).map(String.init)
                if f.count >= 3 {
                    output.append(SelfTestLine(name: f[1], passed: true, detail: f[2]))
                    ok += 1; total += 1
                }
            } else if text.hasPrefix("FAIL|") {
                let f = text.split(separator: "|", maxSplits: 2).map(String.init)
                if f.count >= 3 {
                    output.append(SelfTestLine(name: f[1], passed: false, detail: f[2]))
                    total += 1
                }
            }
        }
        let summary = total == 0 ? "自检未返回结果" : "P0 运行时自检：\(ok)/\(total) 通过"
        return (output, summary)
    }
}