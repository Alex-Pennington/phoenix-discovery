# Phoenix Nest Service Discovery

UDP broadcast-based service discovery for Phoenix Nest SDR programs.

## Overview

This library allows Phoenix Nest programs to automatically find each other on a local network. Every program announces itself via UDP broadcast, and other programs listen for these announcements to build a local service registry.

**Key Features:**
- Zero configuration - just announce and listen
- Automatic service registry with timeout
- Cross-platform (Windows/Linux)
- No external dependencies
- Thread-safe

## Architecture

```
┌─────────────────┐         UDP :5400          ┌─────────────────┐
│  sdr_server     │◄─────────────────────────► │  waterfall      │
│  "helo"         │     broadcast/listen       │  "helo"         │
└─────────────────┘                            └─────────────────┘
```

All programs:
1. Broadcast "helo" on startup
2. Broadcast "helo" every 30-60 seconds (randomized)
3. Broadcast "bye" on shutdown
4. Listen for announcements from other programs

## Building

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

### Windows (MSYS2)
```bash
mkdir build && cd build
cmake -G "MSYS Makefiles" ..
make
```

## Usage

### Basic Example

```c
#include "pn_discovery.h"

// Callback for discovered services
void on_service(const char *id, const char *service,
                const char *ip, int ctrl_port, int data_port,
                const char *caps, bool is_bye, void *userdata) {
    if (is_bye) {
        printf("Service left: %s\n", id);
    } else {
        printf("Found: %s at %s:%d\n", service, ip, ctrl_port);
    }
}

int main() {
    // Initialize
    pn_discovery_init(0);  // 0 = default port 5400
    
    // Start listening
    pn_listen(on_service, NULL);
    
    // Announce ourselves
    pn_announce("MY-APP-1", "my_service", 5000, 0, NULL);
    
    // ... do work ...
    
    // Find a specific service
    const pn_service_t *sdr = pn_find_service("sdr_server");
    if (sdr) {
        printf("SDR server at %s:%d\n", sdr->ip, sdr->ctrl_port);
    }
    
    // Cleanup
    pn_discovery_shutdown();
    return 0;
}
```

## API

### Initialization
```c
int pn_discovery_init(int udp_port);   // 0 for default (5400)
void pn_discovery_shutdown(void);
```

### Announcing
```c
int pn_announce(const char *id, const char *service,
                int ctrl_port, int data_port, const char *caps);
void pn_announce_stop(void);
```

### Listening
```c
typedef void (*pn_service_cb)(const char *id, const char *service,
                              const char *ip, int ctrl_port, int data_port,
                              const char *caps, bool is_bye, void *userdata);

int pn_listen(pn_service_cb callback, void *userdata);
```

### Service Registry
```c
const pn_service_t* pn_find_service(const char *service_type);
const pn_service_t* pn_find_service_by_id(const char *id);
int pn_get_services(pn_service_t *out, int max_count);
int pn_get_service_count(void);
```

## Protocol

### Message Format (JSON)

**helo** - Announce service
```json
{
  "m": "PNSD",
  "v": 1,
  "cmd": "helo",
  "id": "KY4OLB-SDR1",
  "svc": "sdr_server",
  "ip": "192.168.1.10",
  "port": 4535,
  "data": 4536,
  "caps": "rsp2pro,2mhz",
  "ts": 1703193600
}
```

**bye** - Leaving network
```json
{
  "m": "PNSD",
  "v": 1,
  "cmd": "bye",
  "id": "KY4OLB-SDR1",
  "ts": 1703193600
}
```

## Service Types

| Type | Description |
|------|-------------|
| `sdr_server` | SDRplay I/Q source |
| `signal_splitter` | Bandwidth reducer / edge node |
| `signal_relay` | Cloud hub |
| `waterfall` | Display client |
| `controller` | SDR control GUI |
| `detector` | Signal detector |

## Testing

Run two instances to test discovery:

```bash
# Terminal 1 - Server
./test_discovery server KY4OLB-SDR1

# Terminal 2 - Client
./test_discovery client WF1
```

## License

MIT License - (c) 2024 Phoenix Nest LLC
