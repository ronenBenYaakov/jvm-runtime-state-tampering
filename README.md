# JVM Runtime State Tampering & Reference Queue Interceptor

A native JVMTI agent and proof-of-concept demonstrating in-memory runtime tampering. The agent intercepts and redirects JVM `java.lang.ref.Reference` queues in real time to capture leaked payloads, inspect internal task closures, and disarm garbage-collection-driven cleanup routines.

***

## Overview

Modern Java applications and core JDK libraries (`WeakHashMap`, `java.lang.ref.Cleaner`, and NIO `DirectByteBuffer`) rely on internal `ReferenceQueue` notifications to execute post-mortem disposal routines. When the Garbage Collector marks a referent unreachable, the JVM `ReferenceHandler` daemon inspects the reference's internal `queue` field and pushes it to its assigned queue for cleanup.

This project attaches natively via the **JVM Tool Interface (JVMTI)**, traverses live heap instances, and rewrites the private `Reference.queue` field on matching reference objects to an isolated agent-controlled queue. 

As a result:
1. **Cleanup Starvation**: Target application queues never receive death notifications, disabling resource deallocation routines (such as `Unsafe.freeMemory` or map entry evictions).
2. **Payload & Task Interception**: The agent drains the redirected queue to inspect trapped data—including `WeakHashMap` value payloads, off-heap C memory addresses, and `Cleaner` task closures—without modifying application source code or bytecode.

***

## Architecture

```text
 ┌─────────────────────────────────────────────────────────────┐
 │                      JVM Managed Heap                       │
 └──────────────────────────────┬──────────────────────────────┘
                                │ (GC Marks Referent Unreachable)
                                ▼
                   java.lang.ref.Reference
                                │
                        ┌───────┴────────┐
                        │  queue field   │ ◄──── [ JVMTI Native Agent ]
                        └───────┬────────┘        (Mutates field in-place via JNI)
                                │
        ┌───────────────────────┴────────────────────────┐
        │                                                │
        ▼                                                ▼
┌───────────────────────────┐                ┌───────────────────────────┐
│ Target Application Queue  │                │  Agent Interceptor Queue  │
├───────────────────────────┤                ├───────────────────────────┤
│ • Starved of death events │                │ • Captures dead instances │
│ • Cleanups never execute  │                │ • Extracts payload data   │
│ • Unbounded memory leak   │                │ • Inspects task metadata  │
└───────────────────────────┘                └───────────────────────────┘
```

***

## Project Structure

```text
jvm-runtime-state-tampering/
├── CMakeLists.txt              # Dynamic CMake build configuration
├── Main.java                   # High-performance TCP NIO server with off-heap buffers
├── run_server_with_agent.cmd   # Windows launcher script for server + agent
├── send_work.cmd               # Automated TCP workload generator script
├── include/
│   └── agent_core.h            # Shared global definitions and function declarations
└── src/
    ├── agent_entry.cpp         # Agent lifecycle (Agent_OnLoad, VMInit, capabilities)
    └── agent_core.cpp          # Heap scanner, queue mutator, and leak inspector
```

***

## Technical Mechanics

1. **Native Discovery & Tagging**:
   - The agent invokes `IterateOverInstancesOfClass` for `java.lang.ref.Reference`.
   - Discovered objects receive unique 64-bit integer tags and are queried in batches via `GetObjectsWithTags` with JNI capacity guards.
2. **Queue Pointer Redirection**:
   - The agent calls `SetObjectField` to replace the target `Reference.queue` with an isolated dummy `ReferenceQueue` instance created during `VMInit`.
3. **Real-Time Leak Inspection**:
   - An asynchronous agent thread polls the dummy queue using `ReferenceQueue.poll()`.
   - **`WeakHashMap$Entry`**: Reads the private `value` field to inspect retained cache payloads.
   - **`CleanerImpl$PhantomCleanableRef`**: Reads the private `action` runnable to inspect task metadata (e.g., client session IDs).
   - **`DirectByteBuffer$Deallocator`**: Reads the raw `address` and `capacity` fields of off-heap C memory.

***

## Building

### Prerequisites
- C++20 compliant compiler (GCC/MinGW, Clang, or MSVC)
- CMake 3.20+
- JDK 17+ (JDK 21+ recommended)

### Compile the Shared Library
From the project root:
```cmd
mkdir build
cd build
cmake ..
cmake --build . --config Debug
```
*Output: `build/libgc_user.dll` (Windows) or `build/libgc_user.so` (Linux).*

***

## Running the Demonstration

### 1. Start the Server with the Native Agent Attached
```cmd
run_server_with_agent.cmd
```
*Or manually:*
```cmd
javac Main.java
java -agentpath:cmake-build-debug\libgc_user.dll Main
```

### 2. Generate Workload Traffic
In a separate terminal, trigger off-heap allocations:
```cmd
send_work.cmd
```

***

## Example Output

### Server & Agent Console Output
```text
High-Performance NIO Server (Off-Heap Buffer Pool)
PID: 16816
Listening on TCP port 9000...

============================================================
[DISARMED] >>> OVERWROTE Reference.queue ON ALL OBJECTS <<<
[DISARMED] Interceptor queue is now actively capturing leaks.
============================================================

[+] Client connected: /127.0.0.1:49371
[LEAK-INTERCEPTOR] >>> Captured & Disarmed Cleaner Task: Main$ClientSession$BufferCleanupTask
[LEAK-INTERCEPTOR] >>> Captured & Disarmed Cleaner Task: Main$ClientSession$BufferCleanupTask
[LEAK-INTERCEPTOR] >>> Captured & Disarmed Cleaner Task: Main$ClientSession$BufferCleanupTask
[SERVER] Requests: 1 | Cleaned Buffers: 0 | Native Buffers Pending: 10
[SERVER] Requests: 2 | Cleaned Buffers: 0 | Native Buffers Pending: 20
[SERVER] Requests: 3 | Cleaned Buffers: 0 | Native Buffers Pending: 30
```

### Observation:
- `Cleaned Buffers` remains frozen at `0`.
- The agent captures every `BufferCleanupTask` instance as the Garbage Collector discards unreachable sessions.
- `Native Buffers Pending` accumulates continuously, retaining physical off-heap RAM without application-level visibility.

***

## Threat Classification

- **MITRE ATT&CK**: [T1574 (Hijack Execution Flow)](https://attack.mitre.org/techniques/T1574/) & [T1055 (Process Injection)](https://attack.mitre.org/techniques/T1055/)
- **CWE**: [CWE-400 (Uncontrolled Resource Consumption)](https://cwe.mitre.org/data/definitions/400.html) / [CWE-772 (Missing Release of Resource after Effective Lifetime)](https://cwe.mitre.org/data/definitions/772.html)
- **Impact**: In-Memory Denial of Service (DoS), telemetry evasion, and runtime state inspection.

***

## Mitigations

- **Disable Dynamic Agent Attachment**: Launch production JVM instances with `-XX:+DisableAttachMechanism` to prevent external processes from loading dynamic agents at runtime.
- **Enforce Native Access Controls**: Restrict native library loading in modern OpenJDK runtimes using the `--enable-native-access` parameter.
- **Operating System Hardening**: Restrict user access to JVM startup scripts and enforce strict file permissions on application binaries and environment variables.
