# Phoenix Nest Service Discovery - AI Agent Instructions

## Project Overview
UDP broadcast-based service discovery library for Phoenix Nest SDR (Software Defined Radio) programs. Enables zero-configuration service announcement and discovery on local networks using JSON-over-UDP protocol.

## Architecture

**Core Pattern**: Decentralized mesh discovery
- Each program announces itself via UDP broadcast (port 5400)
- All programs listen for announcements and maintain local service registry
- No central coordinator - fully peer-to-peer for endpoint programs
- Thread-based: separate background threads for announcing and listening

**Protocol**: Custom JSON-over-UDP
- Messages: `helo` (announce/heartbeat), `bye` (shutdown)
- Magic: `"PNSD"`, version: `1`
- See [README.md](../README.md#protocol) for full message format

**Platform Abstraction**: Single-file implementation with `#ifdef _WIN32` blocks
- Windows: Winsock2, CRITICAL_SECTION, HANDLE threads
- Linux: BSD sockets, pthread, pthread_mutex
- Platform-specific code isolated in type definitions and helper macros (lines 14-50 of [src/pn_discovery.c](../src/pn_discovery.c))

## Critical Implementation Details

### No External Dependencies
- **Custom JSON parsing**: Simple string-based helpers (`json_add_string`, `json_get_string`, `json_get_int`) in [src/pn_discovery.c](../src/pn_discovery.c#L99-L132)
- Do NOT add external JSON libraries - this is intentional for portability
- JSON is limited to flat key-value pairs for service announcements

### Thread Safety
- Global state in `g_discovery` struct protected by mutex
- Service registry (`g_discovery.services[]`) requires lock for all access
- Always use `mutex_lock/mutex_unlock` macros when accessing shared state
- Pattern: Lock → modify registry → unlock → invoke callback (never hold lock during callback)

### Service Registry Behavior
- Fixed-size array: `PN_MAX_SERVICES` (32) entries
- Services time out if no heartbeat received (implementation detail in source)
- `pn_find_service()` returns pointer valid only until next discovery call
- Must copy service data if persistence needed beyond callback

### Randomized Announce Intervals
- Prevents thundering herd: randomized between `PN_ANNOUNCE_MIN_SEC` (30) and `PN_ANNOUNCE_MAX_SEC` (60)
- See `get_random_interval()` implementation

## Integration Pattern

**Git Submodule**: This library is meant to be used as a submodule, not built standalone.
- Host projects add via `git submodule add`
- CMakeLists.txt provides `pn_discovery` target for parent projects
- No build artifacts are committed - host project builds the library

## Development Testing Only

Building is **only for testing the library itself**:
```bash
mkdir build && cd build
cmake ..
cmake --build .
```
**Important**: Delete `build/` when done - not used in production.

**Test executable**: `test_discovery` (development only)

## Testing Pattern

Use dual-instance testing as shown in [test/test_discovery.c](../test/test_discovery.c):
```bash
# Terminal 1 - Server
./test_discovery server KY4OLB-SDR1

# Terminal 2 - Client  
./test_discovery client WF1
```

**Test modes**: `server` (announces as sdr_server), `client` (announces as waterfall), `listen` (passive)

## Coding Conventions

### Service Type Constants
- Always use `PN_SVC_*` constants defined in [include/pn_discovery.h](../include/pn_discovery.h#L41-L46)
- Types: `sdr_server`, `signal_splitter`, `signal_relay`, `waterfall`, `controller`, `detector`

### Error Handling
- Return `0` on success, `-1` on error (C convention)
- Print errors to stderr with `pn_discovery:` prefix
- No exceptions (C code)

### Naming
- Public API: `pn_` prefix
- Internal functions: `static` with descriptive names
- Types: `_t` suffix (e.g., `pn_service_t`, `socket_t`)

### Platform Macros Pattern
```c
#ifdef _WIN32
    // Windows implementation
#else
    // Linux implementation
#endif
```
Use consistently for all platform-specific code.

## Key Files
- [include/pn_discovery.h](../include/pn_discovery.h): Full public API with detailed comments
- [src/pn_discovery.c](../src/pn_discovery.c): Single-file implementation (~817 lines)
- [test/test_discovery.c](../test/test_discovery.c): Reference usage examples

## Phoenix Nest Context
This library is part of larger SDR system where programs need to discover:
- SDR hardware servers (`sdr_server`) providing I/Q data
- Signal processing nodes (`signal_splitter`, `signal_relay`)  
- Client applications (`waterfall`, `controller`, `detector`)

Design prioritizes simplicity and zero-configuration for ham radio operators.
