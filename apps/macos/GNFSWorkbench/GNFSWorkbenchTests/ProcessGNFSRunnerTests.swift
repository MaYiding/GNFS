import Darwin
import Foundation
import XCTest

@testable import GNFSWorkbench

final class ProcessGNFSRunnerTests: XCTestCase {
  func testPipeFileActionsNeverCloseStandardDescriptors() {
    let plan = ProcessGNFSRunner.pipeFileActionPlan(
      stdoutPipe: [3, 4],
      stderrPipe: [5, 6]
    )

    XCTAssertEqual(
      plan.duplicates,
      [
        .init(source: 4, destination: STDOUT_FILENO),
        .init(source: 6, destination: STDERR_FILENO),
      ]
    )
    XCTAssertEqual(plan.closures, [3, 4, 5, 6])
    XCTAssertTrue(plan.closures.allSatisfy { $0 > STDERR_FILENO })
  }

  func testStdoutEOFDuringSuccessfulCleanupDoesNotInterruptRoot() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("delayed-exit-probe.zsh")
    let script = """
      #!/bin/zsh
      print -r -- '\(Self.successfulResultLine)'
      exec 1>&-
      /bin/sleep 0.2
      exit 0
      """
    try Data(script.utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("workspace", isDirectory: true),
      resumeBase: root.appendingPathComponent("workspace/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let clock = ContinuousClock()
    let started = clock.now
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    var terminalEvent: CLIEvent?
    for try await event in stream where event.type == .result {
      terminalEvent = event
    }
    let elapsed = started.duration(to: clock.now)
    await runner.cancel()

    XCTAssertEqual(terminalEvent?.type, .result)
    XCTAssertGreaterThanOrEqual(elapsed, .milliseconds(180))
  }

  func testRealCLIStreamsVerifiedResultWithoutTerminalParsing() async throws {
    let executable = try XCTUnwrap(Bundle.main.url(forResource: "gnfs", withExtension: nil))
    XCTAssertTrue(FileManager.default.isExecutableFile(atPath: executable.path))

    let resolver = GNFSExecutableResolver(
      bundle: .main,
      environment: [:],
      workingDirectory: repositoryRoot()
    )
    let runner = ProcessGNFSRunner(resolver: resolver)
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "96091"),
      workspace: workspace
    )
    var eventKinds: [CLIEvent.Kind] = []
    var result: FactorizationResult?
    for try await event in stream {
      eventKinds.append(event.type)
      if event.type == .started {
        XCTAssertEqual(event.completeFactorization, true)
      }
      if event.type == .result { result = event.result }
    }
    await runner.cancel()

    XCTAssertEqual(eventKinds.first, .started)
    XCTAssertTrue(eventKinds.contains(.log))
    XCTAssertEqual(eventKinds.last, .result)
    XCTAssertEqual(result?.factors, ["307", "313"])
    XCTAssertEqual(result?.factorizationComplete, true)
    XCTAssertEqual(result?.factorsPrime, true)
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRealCLIProducesCompletePrimeFactorizationWithMultiplicity() async throws {
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    var result: FactorizationResult?
    for try await event in stream where event.type == .result {
      result = event.result
    }
    await runner.cancel()

    XCTAssertEqual(result?.factors, ["2", "2", "2", "3", "3", "5"])
    XCTAssertEqual(result?.distinctPrimeCount, 3)
    XCTAssertEqual(result?.factorExpression, "2^3 × 3^2 × 5")
    XCTAssertEqual(result?.factorizationComplete, true)
    XCTAssertEqual(result?.factorsPrime, true)
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRealCLIAcceptsPrimeInputAsCompleteFactorization() async throws {
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "127"),
      workspace: workspace
    )
    var result: FactorizationResult?
    for try await event in stream where event.type == .result {
      result = event.result
    }
    await runner.cancel()

    XCTAssertEqual(result?.factors, ["127"])
    XCTAssertEqual(result?.factorizationComplete, true)
    XCTAssertEqual(result?.factorsPrime, true)
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRealCLIAcceptsValidatedHexadecimalInput() async throws {
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let workspace = temporaryWorkspace()
    defer { try? FileManager.default.removeItem(at: workspace.directory) }

    let stream = try await runner.start(
      configuration: RunConfiguration(number: "0x1775b"),
      workspace: workspace
    )
    var result: FactorizationResult?
    for try await event in stream where event.type == .result {
      result = event.result
    }
    await runner.cancel()

    XCTAssertEqual(result?.number, "96091")
    XCTAssertEqual(result?.factors, ["307", "313"])
    XCTAssertEqual(result?.isVerified, true)
  }

  func testRunnerUsesWorkspaceDirectoryAndStateResumeBase() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("workspace-probe.zsh")
    let script = #"""
      #!/bin/zsh
      print -r -- "$PWD" > cwd.txt
      print -r -- "$GNFS_RESUME" > resume.txt
      print -r -- '{"schema_version":1,"type":"error","message":"probe complete"}'
      exit 1
      """#
    try Data(script.utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/probe", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/probe/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    for try await _ in stream {}
    await runner.cancel()

    let cwd = try String(
      contentsOf: workspace.directory.appendingPathComponent("cwd.txt"),
      encoding: .utf8
    ).trimmingCharacters(in: .whitespacesAndNewlines)
    let resume = try String(
      contentsOf: workspace.directory.appendingPathComponent("resume.txt"),
      encoding: .utf8
    ).trimmingCharacters(in: .whitespacesAndNewlines)
    XCTAssertEqual(
      URL(fileURLWithPath: cwd).standardizedFileURL, workspace.directory.standardizedFileURL)
    XCTAssertEqual(resume, workspace.resumeBase.path)
  }

  func testCancelReturnsAfterProcessAndReadersHaveStopped() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("cancellation-probe.zsh")
    let script = #"""
      #!/bin/zsh
      print -r -- "$$" > pid.txt
      trap 'exit 0' INT TERM
      while true; do
        print -r -- '{"schema_version":1,"type":"log","level":"INFO","message":"draining"}'
        print -r -- 'diagnostic' >&2
        sleep 0.01
      done
      """#
    try Data(script.utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/cancel", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/cancel/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    let consumer = Task {
      for try await _ in stream {}
    }
    let pidURL = workspace.directory.appendingPathComponent("pid.txt")
    let didLaunch = await waitUntilFileExists(pidURL)
    XCTAssertTrue(didLaunch)

    await runner.cancel()
    try await consumer.value

    let pidText = try String(contentsOf: pidURL, encoding: .utf8)
      .trimmingCharacters(in: .whitespacesAndNewlines)
    let pid = try XCTUnwrap(Int32(pidText))
    XCTAssertEqual(Darwin.kill(pid, 0), -1)
    XCTAssertEqual(errno, ESRCH)
  }

  func testCancelKillsSeparateDescendantProcessGroupsHoldingStderr() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)

    let grandchild = root.appendingPathComponent("grandchild.py")
    let child = root.appendingPathComponent("child.py")
    let executable = root.appendingPathComponent("descendant-cancellation-probe.zsh")
    let grandchildScript = #"""
      #!/usr/bin/env python3
      import os
      import signal
      import time

      os.setpgid(0, 0)
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      workspace = os.path.dirname(os.environ["GNFS_RESUME"])
      with open(os.path.join(workspace, "grandchild.pid"), "w", encoding="utf-8") as output:
          output.write(str(os.getpid()))
      os.write(2, b"grandchild keeps stderr open\n")
      while True:
          time.sleep(1)
      """#
    let childScript = #"""
      #!/usr/bin/env python3
      import os
      import signal
      import subprocess
      import time

      os.setpgid(0, 0)
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      workspace = os.path.dirname(os.environ["GNFS_RESUME"])
      with open(os.path.join(workspace, "child.pid"), "w", encoding="utf-8") as output:
          output.write(str(os.getpid()))
      subprocess.Popen(
          [os.path.join(os.path.dirname(__file__), "grandchild.py")],
          stdout=subprocess.DEVNULL,
      )
      os.write(2, b"child keeps stderr open\n")
      while True:
          time.sleep(1)
      """#
    let rootScript = #"""
      #!/bin/zsh
      trap '' INT TERM
      print -r -- "$$" > "${GNFS_RESUME:h}/root.pid"
      "${0:A:h}/child.py" > /dev/null &
      wait
      """#
    try Data(grandchildScript.utf8).write(to: grandchild)
    try Data(childScript.utf8).write(to: child)
    try Data(rootScript.utf8).write(to: executable)
    for script in [grandchild, child, executable] {
      try FileManager.default.setAttributes(
        [.posixPermissions: 0o755],
        ofItemAtPath: script.path
      )
    }

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/descendant-cancel", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/descendant-cancel/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    let consumer = Task {
      for try await _ in stream {}
    }
    let rootPID = workspace.directory.appendingPathComponent("root.pid")
    let childPID = workspace.directory.appendingPathComponent("child.pid")
    let grandchildPID = workspace.directory.appendingPathComponent("grandchild.pid")
    let rootDidStart = await waitUntilFileExists(rootPID, timeout: .seconds(5))
    let childDidStart = await waitUntilFileExists(childPID, timeout: .seconds(15))
    let grandchildDidStart = await waitUntilFileExists(grandchildPID, timeout: .seconds(15))
    XCTAssertTrue(rootDidStart, "root PID did not appear within 5s: \(rootPID.path)")
    XCTAssertTrue(childDidStart, "child PID did not appear within 15s: \(childPID.path)")
    XCTAssertTrue(
      grandchildDidStart,
      "grandchild PID did not appear within 15s: \(grandchildPID.path)"
    )

    let clock = ContinuousClock()
    let cancellationStarted = clock.now
    await runner.cancel()
    try await consumer.value

    XCTAssertLessThan(cancellationStarted.duration(to: clock.now), .seconds(5))
    for pidURL in [rootPID, childPID, grandchildPID] {
      let didStop = await waitUntilProcessStops(pidURL: pidURL)
      XCTAssertTrue(
        didStop,
        "process did not stop within 5s: \(pidURL.path); \(processStateDescription(pidURL: pidURL))"
      )
    }
  }

  func testCancelReapsDescendantSpawnedAfterInterruptPhaseDeadline() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)

    let child = root.appendingPathComponent("interrupt-child.py")
    let executable = root.appendingPathComponent("interrupt-parent.py")
    let childScript = #"""
      #!/usr/bin/env python3
      import os
      import signal
      import time

      os.setpgid(0, 0)
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      workspace = os.path.dirname(os.environ["GNFS_RESUME"])
      with open(os.path.join(workspace, "interrupt-child.pid"), "w", encoding="utf-8") as output:
          output.write(str(os.getpid()))
      os.write(2, b"interrupt child keeps both pipes open\n")
      while True:
          time.sleep(1)
      """#
    let rootScript = #"""
      #!/usr/bin/env python3
      import os
      import signal
      import subprocess
      import time

      workspace = os.path.dirname(os.environ["GNFS_RESUME"])
      child_path = os.path.join(os.path.dirname(__file__), "interrupt-child.py")

      def on_interrupt(_signum, _frame):
          with open(os.path.join(workspace, "interrupt-handler-entered"), "w", encoding="utf-8") as output:
              output.write("SIGINT")
          time.sleep(0.45)
          child = subprocess.Popen([child_path])
          with open(os.path.join(workspace, "interrupt-child.pid"), "w", encoding="utf-8") as output:
              output.write(str(child.pid))
          os._exit(130)

      signal.signal(signal.SIGINT, on_interrupt)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      with open(os.path.join(workspace, "interrupt-root.pid"), "w", encoding="utf-8") as output:
          output.write(str(os.getpid()))
      while True:
          time.sleep(1)
      """#
    try Data(childScript.utf8).write(to: child)
    try Data(rootScript.utf8).write(to: executable)
    for script in [child, executable] {
      try FileManager.default.setAttributes(
        [.posixPermissions: 0o755],
        ofItemAtPath: script.path
      )
    }

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/interrupt-cancel", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/interrupt-cancel/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    let consumer = Task {
      for try await _ in stream {}
    }
    let rootPID = workspace.directory.appendingPathComponent("interrupt-root.pid")
    let rootDidStart = await waitUntilFileExists(rootPID)
    XCTAssertTrue(rootDidStart)

    let clock = ContinuousClock()
    let cancellationStarted = clock.now
    await runner.cancel()
    try await consumer.value
    XCTAssertLessThan(cancellationStarted.duration(to: clock.now), .seconds(5))

    let childPID = workspace.directory.appendingPathComponent("interrupt-child.pid")
    XCTAssertTrue(FileManager.default.fileExists(
      atPath: workspace.directory.appendingPathComponent("interrupt-handler-entered").path
    ))
    for pidURL in [rootPID, childPID] {
      let didStop = await waitUntilProcessStops(pidURL: pidURL)
      XCTAssertTrue(didStop, processStateDescription(pidURL: pidURL))
    }
  }

  func testRootCrashReapsTrackedDescendantGroupsHoldingStderrWithoutCancel() async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)

    let grandchild = root.appendingPathComponent("crash-grandchild.py")
    let child = root.appendingPathComponent("crash-child.py")
    let executable = root.appendingPathComponent("crash-root.zsh")
    let grandchildScript = #"""
      #!/usr/bin/env python3
      import os
      import signal
      import time

      os.setpgid(0, 0)
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      workspace = os.path.dirname(os.environ["GNFS_RESUME"])
      with open(os.path.join(workspace, "crash-grandchild.pid"), "w", encoding="utf-8") as output:
          output.write(str(os.getpid()))
      with open(os.path.join(workspace, "crash-grandchild.sid"), "w", encoding="utf-8") as output:
          output.write(str(os.getsid(0)))
      os.write(2, b"crash grandchild keeps stderr open\n")
      while True:
          time.sleep(1)
      """#
    let childScript = #"""
      #!/usr/bin/env python3
      import os
      import signal
      import subprocess
      import time

      os.setpgid(0, 0)
      signal.signal(signal.SIGINT, signal.SIG_IGN)
      signal.signal(signal.SIGTERM, signal.SIG_IGN)
      workspace = os.path.dirname(os.environ["GNFS_RESUME"])
      with open(os.path.join(workspace, "crash-child.pid"), "w", encoding="utf-8") as output:
          output.write(str(os.getpid()))
      with open(os.path.join(workspace, "crash-child.sid"), "w", encoding="utf-8") as output:
          output.write(str(os.getsid(0)))
      subprocess.Popen(
          [os.path.join(os.path.dirname(__file__), "crash-grandchild.py")],
          stdout=subprocess.DEVNULL,
      )
      os.write(2, b"crash child keeps stderr open\n")
      while True:
          time.sleep(1)
      """#
    let rootScript = #"""
      #!/bin/zsh
      print -r -- "$$" > "${GNFS_RESUME:h}/crash-root.pid"
      /usr/bin/env python3 -c 'import os; print(os.getsid(0))' \
        > "${GNFS_RESUME:h}/crash-root.sid"
      "${0:A:h}/crash-child.py" &
      repeat 500 {
        if [[ -s "${GNFS_RESUME:h}/crash-child.pid" && \
              -s "${GNFS_RESUME:h}/crash-grandchild.pid" ]]; then
          exit 9
        fi
        sleep 0.01
      }
      exit 10
      """#
    try Data(grandchildScript.utf8).write(to: grandchild)
    try Data(childScript.utf8).write(to: child)
    try Data(rootScript.utf8).write(to: executable)
    for script in [grandchild, child, executable] {
      try FileManager.default.setAttributes(
        [.posixPermissions: 0o755],
        ofItemAtPath: script.path
      )
    }

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("Runs/crash", isDirectory: true),
      resumeBase: root.appendingPathComponent("Runs/crash/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )

    let clock = ContinuousClock()
    let started = clock.now
    do {
      for try await _ in stream {}
      XCTFail("root crash without a terminal event should fail")
    } catch let error as GNFSRunnerError {
      guard case .unexpectedTermination(9, let stderr) = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
      XCTAssertTrue(stderr.contains("keeps stderr open"))
    }
    XCTAssertLessThan(started.duration(to: clock.now), .seconds(4))

    let rootPID = try XCTUnwrap(readProcessID(
      at: workspace.directory.appendingPathComponent("crash-root.pid")
    ))
    for sessionName in ["crash-root.sid", "crash-child.sid", "crash-grandchild.sid"] {
      let sessionID = try XCTUnwrap(readProcessID(
        at: workspace.directory.appendingPathComponent(sessionName)
      ))
      XCTAssertEqual(sessionID, rootPID, "process escaped the isolated launch session")
    }

    for pidName in ["crash-root.pid", "crash-child.pid", "crash-grandchild.pid"] {
      let didStop = await waitUntilProcessStops(
        pidURL: workspace.directory.appendingPathComponent(pidName)
      )
      XCTAssertTrue(didStop, "tracked crash process survived: \(pidName)")
    }
  }

  func testSuccessResultRequiresZeroExitStatus() async throws {
    do {
      _ = try await runProbe(lines: [Self.successfulResultLine], exitStatus: 1)
      XCTFail("success result followed by exit 1 should fail")
    } catch let error as GNFSRunnerError {
      guard case .terminalStatusMismatch(expected: 0, actual: 1, _) = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
    }
  }

  func testFailedResultRequiresAndAcceptsExitOne() async throws {
    let events = try await runProbe(lines: [Self.failedResultLine], exitStatus: 1)
    XCTAssertEqual(events.map(\.type), [.result])
    XCTAssertEqual(events.first?.result?.success, false)
  }

  func testErrorRequiresAndAcceptsExitOne() async throws {
    let errorLine = #"{"schema_version":1,"type":"error","code":"probe","message":"failed"}"#
    let events = try await runProbe(lines: [errorLine], exitStatus: 1)
    XCTAssertEqual(events.map(\.type), [.error])

    do {
      _ = try await runProbe(lines: [errorLine], exitStatus: 0)
      XCTFail("error event followed by exit 0 should fail")
    } catch let error as GNFSRunnerError {
      guard case .terminalStatusMismatch(expected: 1, actual: 0, _) = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
    }
  }

  func testRejectsDuplicateOrPostTerminalOutput() async throws {
    let errorLine = #"{"schema_version":1,"type":"error","code":"probe","message":"failed"}"#
    let postTerminalLog = #"{"schema_version":1,"type":"log","level":"INFO","message":"late"}"#
    do {
      _ = try await runProbe(lines: [errorLine, postTerminalLog], exitStatus: 1)
      XCTFail("output after terminal event should fail")
    } catch let error as GNFSRunnerError {
      guard case .protocolViolation = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
    }

    do {
      _ = try await runProbe(lines: [errorLine, errorLine], exitStatus: 1)
      XCTFail("duplicate terminal events should fail")
    } catch let error as GNFSRunnerError {
      guard case .protocolViolation = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
    }
  }

  func testRequiresTerminalEventBeforeEOF() async throws {
    let logLine = #"{"schema_version":1,"type":"log","level":"INFO","message":"only-log"}"#
    do {
      _ = try await runProbe(lines: [logLine], exitStatus: 0)
      XCTFail("EOF without terminal event should fail")
    } catch let error as GNFSRunnerError {
      guard case .unexpectedTermination(0, _) = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
    }
  }

  func testStdoutBufferDropsOldLogsButRetainsVerifiedTerminal() async throws {
    let logLine = #"{"schema_version":1,"type":"log","level":"INFO","message":"flood"}"#
    let events = try await runProbe(
      lines: [logLine, Self.successfulResultLine],
      exitStatus: 0,
      repeatedFirstLineCount: 4_000,
      waitForProducerExitBeforeConsumption: true
    )

    // The consumer may overlap with the final reader drain, so its lifetime
    // total can exceed the instantaneous 512-event backlog. It must still
    // observe that older logs were dropped and that the terminal survived.
    XCTAssertLessThan(events.count, 4_001)
    XCTAssertEqual(events.last?.type, .result)
    XCTAssertEqual(events.last?.result?.success, true)
  }

  func testStderrDiagnosticsAreBoundedToTail() async throws {
    let diagnostic = String(repeating: "diagnostic-tail-", count: 64)
    do {
      _ = try await runProbe(
        lines: [],
        exitStatus: 7,
        stderrLines: Array(repeating: diagnostic, count: 256)
      )
      XCTFail("non-zero exit without terminal should fail")
    } catch let error as GNFSRunnerError {
      guard case .unexpectedTermination(7, let stderr) = error else {
        return XCTFail("unexpected runner error: \(error)")
      }
      XCTAssertTrue(stderr.hasPrefix("[stderr truncated;"))
      XCTAssertLessThanOrEqual(
        stderr.utf8.count,
        ProcessGNFSRunner.diagnosticTailLimit + 80
      )
      XCTAssertTrue(stderr.contains("diagnostic-tail-"))
    }
  }

  func testOversizedEventLineFailsQuicklyWithAndWithoutNewline() async throws {
    for terminatesLine in [false, true] {
      let clock = ContinuousClock()
      let started = clock.now
      do {
        try await runOversizedLineProbe(terminatesLine: terminatesLine)
        XCTFail("oversized event line should fail")
      } catch let error as GNFSRunnerError {
        guard case .protocolViolation(let message) = error else {
          return XCTFail("unexpected runner error: \(error)")
        }
        XCTAssertTrue(message.contains("超过"))
        XCTAssertLessThan(started.duration(to: clock.now), .seconds(4))
      }
    }
  }

  @MainActor
  func testAppModelCancelsRunningBundledCLIWithoutReportingFailure() async throws {
    let storeDirectory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: storeDirectory) }

    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: [:],
        workingDirectory: repositoryRoot()
      ))
    let model = AppModel(runner: runner, store: RunStore(baseURL: storeDirectory))
    model.draftConfiguration = RunConfiguration(
      number:
        "1522605027922533360535618378132637429718068114961380688657908494580122963258952897654000350692006139",
      method: .gnfs
    )

    await model.startRun()
    let didStart = await waitUntil { model.activeRun?.selectedMethod == .gnfs }
    XCTAssertTrue(didStart)
    model.cancelRun()

    let didCancel = await waitUntil { model.activeRun?.status == .cancelled }
    XCTAssertTrue(didCancel)
    XCTAssertEqual(model.activeRun?.errorMessage, "任务已由用户取消。")
  }

  private func repositoryRoot() -> URL {
    var directory = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    for _ in 0..<4 { directory.deleteLastPathComponent() }
    return directory
  }

  private func temporaryWorkspace() -> RunWorkspace {
    let directory = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    return RunWorkspace(
      directory: directory,
      resumeBase: directory.appendingPathComponent("state")
    )
  }

  private func runProbe(
    lines: [String],
    exitStatus: Int32,
    stderrLines: [String] = [],
    repeatedFirstLineCount: Int? = nil,
    waitForProducerExitBeforeConsumption: Bool = false,
    delayBeforeConsumption: Duration? = nil
  ) async throws -> [CLIEvent] {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("event-probe.zsh")
    var scriptLines = ["#!/bin/zsh", "print -r -- \"$$\" > producer.pid"]
    if let repeatedFirstLineCount, let firstLine = lines.first {
      scriptLines.append("repeat \(repeatedFirstLineCount) print -r -- '\(firstLine)'")
      scriptLines += lines.dropFirst().map { "print -r -- '\($0)'" }
    } else {
      scriptLines += lines.map { "print -r -- '\($0)'" }
    }
    scriptLines += stderrLines.map { "print -r -- '\($0)' >&2" }
    scriptLines.append("exit \(exitStatus)")
    try Data((scriptLines.joined(separator: "\n") + "\n").utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("workspace", isDirectory: true),
      resumeBase: root.appendingPathComponent("workspace/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    if waitForProducerExitBeforeConsumption {
      let didStop = await waitUntilProcessStops(
        pidURL: workspace.directory.appendingPathComponent("producer.pid")
      )
      XCTAssertTrue(didStop)
    }
    if let delayBeforeConsumption {
      try await Task.sleep(for: delayBeforeConsumption)
    }

    var events: [CLIEvent] = []
    do {
      for try await event in stream {
        events.append(event)
      }
    } catch {
      await runner.cancel()
      throw error
    }
    await runner.cancel()
    return events
  }

  private func runOversizedLineProbe(terminatesLine: Bool) async throws {
    let root = FileManager.default.temporaryDirectory
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    defer { try? FileManager.default.removeItem(at: root) }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    let executable = root.appendingPathComponent("oversized-event-probe.zsh")
    let newlineExpression = terminatesLine ? #"+ "\n""# : ""
    let script = """
      #!/bin/zsh
      /usr/bin/env python3 -c 'import sys; sys.stdout.write("x" * \(ProcessGNFSRunner.maxEventLineBytes + 1) \(newlineExpression)); sys.stdout.flush()'
      while true; do :; done
      """
    try Data(script.utf8).write(to: executable)
    try FileManager.default.setAttributes(
      [.posixPermissions: 0o755],
      ofItemAtPath: executable.path
    )

    let workspace = RunWorkspace(
      directory: root.appendingPathComponent("workspace", isDirectory: true),
      resumeBase: root.appendingPathComponent("workspace/state")
    )
    let runner = ProcessGNFSRunner(
      resolver: GNFSExecutableResolver(
        bundle: .main,
        environment: ["GNFS_CLI_PATH": executable.path],
        workingDirectory: root
      ))
    let stream = try await runner.start(
      configuration: RunConfiguration(number: "360"),
      workspace: workspace
    )
    do {
      for try await _ in stream {}
    } catch {
      await runner.cancel()
      throw error
    }
    await runner.cancel()
  }

  private func waitUntilFileExists(
    _ url: URL,
    timeout: Duration = .seconds(2)
  ) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if FileManager.default.fileExists(atPath: url.path) { return true }
      try? await Task.sleep(for: .milliseconds(10))
    }
    return FileManager.default.fileExists(atPath: url.path)
  }

  private func waitUntilProcessStops(
    pidURL: URL,
    timeout: Duration = .seconds(5)
  ) async -> Bool {
    guard await waitUntilFileExists(pidURL, timeout: timeout),
      let pidText = try? String(contentsOf: pidURL, encoding: .utf8)
        .trimmingCharacters(in: .whitespacesAndNewlines),
      let pid = Int32(pidText)
    else {
      return false
    }

    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if processHasStopped(pid) { return true }
      try? await Task.sleep(for: .milliseconds(10))
    }
    return processHasStopped(pid)
  }

  private func processHasStopped(_ pid: Int32) -> Bool {
    if Darwin.kill(pid, 0) == -1, errno == ESRCH { return true }
    var processInfo = proc_bsdinfo()
    let expectedSize = Int32(MemoryLayout<proc_bsdinfo>.size)
    let result = withUnsafeMutablePointer(to: &processInfo) { pointer in
      proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, pointer, expectedSize)
    }
    return result != expectedSize || processInfo.pbi_status == UInt32(SZOMB)
  }

  private func processStateDescription(pidURL: URL) -> String {
    guard let pid = readProcessID(at: pidURL) else {
      return "process PID file was not created: \(pidURL.lastPathComponent)"
    }
    var processInfo = proc_bsdinfo()
    let expectedSize = Int32(MemoryLayout<proc_bsdinfo>.size)
    let result = withUnsafeMutablePointer(to: &processInfo) { pointer in
      proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, pointer, expectedSize)
    }
    guard result == expectedSize else { return "process \(pid) has no BSD info" }
    return "process \(pid) is still present with status \(processInfo.pbi_status)"
  }

  private func readProcessID(at url: URL) -> Int32? {
    guard let contents = try? String(contentsOf: url, encoding: .utf8) else { return nil }
    return Int32(contents.trimmingCharacters(in: .whitespacesAndNewlines))
  }

  private static let successfulResultLine =
    #"{"schema_version":1,"type":"result","result":{"success":true,"factorization_complete":true,"factors_prime":true,"n":"360","n_bits":9,"n_digits":3,"method":"trial","method_name":"Trial Division","method_reason":"probe","factors":["2","2","2","3","3","5"],"timings":{"total_s":0.01,"poly_s":0,"fb_s":0,"sieve_s":0,"filter_s":0,"linalg_s":0,"sqrt_s":0,"extract_s":0.01},"stats":{"degree":0,"rational_bound":0,"algebraic_bound":0,"large_prime_bound":0,"rational_primes":0,"algebraic_primes":0,"special_q_processed":0,"candidates_total":0,"relations_found":0,"full_relations":0,"partial_1lp":0,"partial_2lp":0,"relations_after_filter":0,"singletons_removed":0,"merged_relations":0,"matrix_rows":0,"matrix_cols":0,"matrix_excess":0,"dependencies_found":0,"dependencies_tried":0}}}"#

  private static let failedResultLine = successfulResultLine.replacingOccurrences(
    of: #""success":true"#,
    with: #""success":false"#
  )

  @MainActor
  private func waitUntil(
    timeout: Duration = .seconds(5),
    condition: @escaping @MainActor () -> Bool
  ) async -> Bool {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
      if condition() { return true }
      try? await Task.sleep(for: .milliseconds(20))
    }
    return condition()
  }
}
