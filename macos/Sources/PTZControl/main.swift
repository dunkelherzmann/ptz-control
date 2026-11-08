import AppKit
import PTZCore

// ssh runs this executable as SSH_ASKPASS; relay its prompt and exit before any UI.
if ptz_is_askpass_invocation() != 0 {
    exit(ptz_askpass_main(CommandLine.argc, CommandLine.unsafeArgv))
}

MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    // A snapshot test copy stays out of the Dock and never takes focus.
    app.setActivationPolicy(ProcessInfo.processInfo.environment["PTZ_SNAPSHOT_PATH"] == nil ? .regular : .prohibited)
    withExtendedLifetime(delegate) { app.run() }
}
