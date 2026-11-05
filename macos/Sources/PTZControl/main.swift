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
    app.setActivationPolicy(.regular)
    withExtendedLifetime(delegate) { app.run() }
}
