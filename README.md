# JVM Reference Queue Interceptor & Disarmer

A native JVMTI agent that dynamically intercepts and mutates active `java.lang.ref.Reference` objects in-memory to redirect reachability notifications away from target reference queues.

***

## Overview

In Java, classes like `PhantomReference`, `WeakReference`, and `SoftReference` can register with a `ReferenceQueue`. When the Garbage Collector marks a referent unreachable, the internal JVM `ReferenceHandler` daemon checks the reference's internal `queue` field and pushes the reference into its assigned queue.

This agent runs inside the JVM process space with native capabilities. It continuously scans the JVM heap for live `java.lang.ref.Reference` instances and replaces their target `queue` with an isolated dummy queue, effectively silencing reachability notifications and preventing cleanup routines or survivor counters from firing.

***

## Architecture

```text
       [ JVM Heap Objects ]
                │
         (GC discovers)
                │
                ▼
      java.lang.ref.Reference
                │
        ┌───────┴────────┐
        │  queue field   │ ◄──── [ JVMTI Disarmer Agent ]
        └───────┬────────┘        (Rewrites field to dummy queue)
                │
        ┌───────┴────────────────────────┐
        │                                │
        ▼                                ▼
[ Original ReferenceQueue ]     [ Dummy ReferenceQueue ]
   (Frozen / Silenced)            (Disarmed / Dropped)
```

- **`agent_entry.cpp`**: Registers the agent with the JVM on startup (`Agent_OnLoad`), enables object-tagging capabilities, and handles the `VMInit` callback to create the dummy queue singleton.
- **`agent_core.cpp`**: Spawns a background thread that periodically scans heap references using `IterateOverInstancesOfClass` and mutates `Reference.queue` fields via JNI.
- **`agent_core.h`**: Defines the shared global references, atomic state flags, and interface declarations.

***

## How It Works

1. **Initialization (`VMInit`)**:
    - Locates `java.lang.ref.Reference` and obtains the `queue` field ID.
    - Instantiates a private, isolated dummy `ReferenceQueue` instance.
    - Spawns a dedicated native background monitoring thread.

2. **Heap Scanning & Tagging**:
    - The native thread invokes `IterateOverInstancesOfClass` to discover all allocated reference objects in memory.
    - Assigns unique 64-bit integer tags to each instance to avoid duplicate processing across GC cycles.

3. **Field Mutation**:
    - Uses `GetObjectsWithTags` to acquire JNI handles to the discovered reference objects.
    - Calls `SetObjectField` on every instance to reassign `Reference.queue` to the dummy queue.

4. **Result**:
    - Any pending cleaner threads or resource disposal mechanisms listening to the application's original queue stop receiving events.
    - Counters tracking reference enqueue events become frozen.

***

## Running the Agent

### Command-Line Usage

Attach the compiled agent library to any Java process using the `-agentpath` parameter at startup:

#### Windows
```cmd
java -agentpath:path\to\libgc_user.dll Main
```

#### Linux
```bash
java -agentpath:path/to/libgc_user.so Main
```

***

## Example Output

When running with an active producer/cleaner workload:

```text
Java process started.
PID: 23180
Workers started.
Waiting...
Live map entries: 9 | survivors: 32
Live map entries: 19 | survivors: 64

============================================================
[DISARMED] >>> OVERWROTE Reference.queue ON ALL OBJECTS <<<
[DISARMED] Cleanups redirected to dummy/null queue.
============================================================

Live map entries: 28 | survivors: 64
Live map entries: 38 | survivors: 64
Live map entries: 47 | survivors: 64
```

***

## Use Cases

- **Runtime Introspection & Debugging**: Analyzing how reference queues and `Cleaner` mechanisms behave when reachability signals fail.
- **Garbage Collection Stress Testing**: Simulating reference-handling starvation and memory leak scenarios without modifying application source code.
- **JVM Security & Sandboxing Research**: Demonstrating the security boundary and internal privileges of native agents within the HotSpot VM.