/*
 * Phoenix Nest Service Discovery
 * 
 * UDP broadcast-based service discovery for LAN mesh networking.
 * Every program announces itself, programs sort out who needs what.
 * 
 * NOTE: TCP coordinator functions exist but are ONLY for use by
 * signal_splitter (edge node) and signal_relay (cloud hub).
 * Endpoint programs (sdr_server, waterfall, etc.) use UDP ONLY.
 * 
 * (c) 2024 Phoenix Nest LLC
 * License: MIT
 */

#ifndef PN_DISCOVERY_H
#define PN_DISCOVERY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default ports */
#define PN_DISCOVERY_UDP_PORT   5400
#define PN_DISCOVERY_TCP_PORT   5401

/* Limits */
#define PN_MAX_ID_LEN           64
#define PN_MAX_SERVICE_LEN      32
#define PN_MAX_IP_LEN           64
#define PN_MAX_CAPS_LEN         128
#define PN_MAX_SERVICES         32

/* Announce interval (randomized between these values) */
#define PN_ANNOUNCE_MIN_SEC     30
#define PN_ANNOUNCE_MAX_SEC     60

/* Service types */
#define PN_SVC_SDR_SERVER       "sdr_server"
#define PN_SVC_SIGNAL_SPLITTER  "signal_splitter"
#define PN_SVC_SIGNAL_RELAY     "signal_relay"
#define PN_SVC_WATERFALL        "waterfall"
#define PN_SVC_CONTROLLER       "controller"
#define PN_SVC_DETECTOR         "detector"

/* Service information */
typedef struct {
    char id[PN_MAX_ID_LEN];           /* Unique instance ID (e.g., "KY4OLB-SDR1") */
    char service[PN_MAX_SERVICE_LEN]; /* Service type (e.g., "sdr_server") */
    char ip[PN_MAX_IP_LEN];           /* IP address */
    int  ctrl_port;                   /* Control/command port */
    int  data_port;                   /* Data port (0 if none) */
    char caps[PN_MAX_CAPS_LEN];       /* Capabilities string */
    uint32_t last_seen;               /* Unix timestamp of last announcement */
    bool active;                      /* Entry in use */
} pn_service_t;

/*
 * Service discovery callback
 * Called when a service is discovered or leaves the network.
 * 
 * @param id        Unique instance ID
 * @param service   Service type
 * @param ip        IP address of the service
 * @param ctrl_port Control port
 * @param data_port Data port (0 if none)
 * @param caps      Capabilities string
 * @param is_bye    true if service is leaving (bye message)
 * @param userdata  User-provided context
 */
typedef void (*pn_service_cb)(const char *id, const char *service,
                              const char *ip, int ctrl_port, int data_port,
                              const char *caps, bool is_bye, void *userdata);

/*
 * Initialize discovery system
 * 
 * @param udp_port  UDP port to use (0 for default 5400)
 * @return 0 on success, -1 on error
 */
int pn_discovery_init(int udp_port);

/*
 * Start announcing this service
 * Broadcasts immediately, then every 30-60 seconds (randomized).
 * 
 * @param id        Unique instance ID (e.g., "KY4OLB-SDR1")
 * @param service   Service type (e.g., PN_SVC_SDR_SERVER)
 * @param ctrl_port Control/command port
 * @param data_port Data port (0 if none)
 * @param caps      Capabilities string (can be NULL)
 * @return 0 on success, -1 on error
 */
int pn_announce(const char *id, const char *service,
                int ctrl_port, int data_port, const char *caps);

/*
 * Stop announcing (sends "bye" message)
 */
void pn_announce_stop(void);

/*
 * Start listening for service announcements
 * Runs in background thread, calls callback for each service found.
 * 
 * @param callback  Function to call when service discovered/leaves
 * @param userdata  User context passed to callback
 * @return 0 on success, -1 on error
 */
int pn_listen(pn_service_cb callback, void *userdata);

/*
 * Find a discovered service by type
 * Returns first matching active service, or NULL if not found.
 * 
 * @param service_type  Service type to find (e.g., PN_SVC_SDR_SERVER)
 * @return Pointer to service info (valid until next discovery call), or NULL
 */
const pn_service_t* pn_find_service(const char *service_type);

/*
 * Find a discovered service by ID
 * 
 * @param id  Unique instance ID to find
 * @return Pointer to service info (valid until next discovery call), or NULL
 */
const pn_service_t* pn_find_service_by_id(const char *id);

/*
 * Get all discovered services
 * 
 * @param out       Array to fill with service info
 * @param max_count Maximum number of services to return
 * @return Number of services copied to array
 */
int pn_get_services(pn_service_t *out, int max_count);

/*
 * Get count of active services
 * 
 * @return Number of active services in registry
 */
int pn_get_service_count(void);

/*
 * Shutdown discovery system
 * Sends "bye" if announcing, stops listener thread, frees resources.
 */
void pn_discovery_shutdown(void);

/*
 * Get local IP address
 * Returns the IP address we're broadcasting from.
 * 
 * @param out     Buffer to receive IP address string
 * @param maxlen  Size of buffer
 * @return 0 on success, -1 on error
 */
int pn_get_local_ip(char *out, int maxlen);

#ifdef __cplusplus
}
#endif

#endif /* PN_DISCOVERY_H */
