/*
 * Phoenix Nest Service Discovery - Implementation
 * 
 * (c) 2024 Phoenix Nest LLC
 * License: MIT
 */

#include "pn_discovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <iphlpapi.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #pragma comment(lib, "iphlpapi.lib")
    typedef SOCKET socket_t;
    typedef HANDLE thread_t;
    typedef CRITICAL_SECTION mutex_t;
    #define INVALID_SOCK INVALID_SOCKET
    #define close_socket closesocket
    #define sleep_ms(ms) Sleep(ms)
    #define mutex_init(m) InitializeCriticalSection(m)
    #define mutex_lock(m) EnterCriticalSection(m)
    #define mutex_unlock(m) LeaveCriticalSection(m)
    #define mutex_destroy(m) DeleteCriticalSection(m)
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <ifaddrs.h>
    #include <pthread.h>
    #include <errno.h>
    typedef int socket_t;
    typedef pthread_t thread_t;
    typedef pthread_mutex_t mutex_t;
    #define INVALID_SOCK -1
    #define close_socket close
    #define sleep_ms(ms) usleep((ms) * 1000)
    #define mutex_init(m) pthread_mutex_init(m, NULL)
    #define mutex_lock(m) pthread_mutex_lock(m)
    #define mutex_unlock(m) pthread_mutex_unlock(m)
    #define mutex_destroy(m) pthread_mutex_destroy(m)
#endif

/* Protocol constants */
#define PN_MAGIC        "PNSD"
#define PN_VERSION      1
#define PN_MAX_MSG_LEN  1024

/* Global state */
static struct {
    bool initialized;
    int udp_port;
    socket_t sock;
    
    /* Announcing */
    bool announcing;
    pn_service_t my_service;
    thread_t announce_thread;
    bool announce_running;
    
    /* Listening */
    bool listening;
    pn_service_cb callback;
    void *callback_userdata;
    thread_t listen_thread;
    bool listen_running;
    
    /* Service registry */
    pn_service_t services[PN_MAX_SERVICES];
    mutex_t services_mutex;
    
    /* Local IP */
    char local_ip[PN_MAX_IP_LEN];
} g_discovery = {0};

/* Forward declarations */
static int build_helo_message(char *buf, int maxlen);
static int build_bye_message(char *buf, int maxlen);
static int parse_message(const char *buf, int len, const char *sender_ip);
static void broadcast_message(const char *msg, int len);
static int get_random_interval(void);
#ifdef _WIN32
static DWORD WINAPI announce_thread_func(LPVOID param);
static DWORD WINAPI listen_thread_func(LPVOID param);
#else
static void* announce_thread_func(void *param);
static void* listen_thread_func(void *param);
#endif

/* Simple JSON helpers (no external dependency) */
static int json_add_string(char *buf, int pos, int maxlen, const char *key, const char *val, bool comma) {
    int n = snprintf(buf + pos, maxlen - pos, "%s\"%s\":\"%s\"", comma ? "," : "", key, val ? val : "");
    return (n > 0 && pos + n < maxlen) ? pos + n : -1;
}

static int json_add_int(char *buf, int pos, int maxlen, const char *key, int val, bool comma) {
    int n = snprintf(buf + pos, maxlen - pos, "%s\"%s\":%d", comma ? "," : "", key, val);
    return (n > 0 && pos + n < maxlen) ? pos + n : -1;
}

static const char* json_get_string(const char *json, const char *key, char *out, int maxlen) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *start = strstr(json, search);
    if (!start) return NULL;
    start += strlen(search);
    const char *end = strchr(start, '"');
    if (!end) return NULL;
    int len = (int)(end - start);
    if (len >= maxlen) len = maxlen - 1;
    strncpy(out, start, len);
    out[len] = '\0';
    return out;
}

static int json_get_int(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *start = strstr(json, search);
    if (!start) return 0;
    start += strlen(search);
    return atoi(start);
}

/* Initialize discovery system */
int pn_discovery_init(int udp_port) {
    if (g_discovery.initialized) {
        return 0;  /* Already initialized */
    }
    
    memset(&g_discovery, 0, sizeof(g_discovery));
    g_discovery.udp_port = (udp_port > 0) ? udp_port : PN_DISCOVERY_UDP_PORT;
    
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "pn_discovery: WSAStartup failed\n");
        return -1;
    }
#endif
    
    /* Create UDP socket */
    g_discovery.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_discovery.sock == INVALID_SOCK) {
        fprintf(stderr, "pn_discovery: socket() failed\n");
        return -1;
    }
    
    /* Enable broadcast */
    int broadcast = 1;
    if (setsockopt(g_discovery.sock, SOL_SOCKET, SO_BROADCAST, 
                   (const char*)&broadcast, sizeof(broadcast)) < 0) {
        fprintf(stderr, "pn_discovery: setsockopt(SO_BROADCAST) failed\n");
        close_socket(g_discovery.sock);
        return -1;
    }
    
    /* Enable address reuse */
    int reuse = 1;
    setsockopt(g_discovery.sock, SOL_SOCKET, SO_REUSEADDR, 
               (const char*)&reuse, sizeof(reuse));
    
    /* Bind to port */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_discovery.udp_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(g_discovery.sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "pn_discovery: bind() failed on port %d\n", g_discovery.udp_port);
        close_socket(g_discovery.sock);
        return -1;
    }
    
    /* Get local IP */
    pn_get_local_ip(g_discovery.local_ip, sizeof(g_discovery.local_ip));
    
    /* Initialize mutex */
    mutex_init(&g_discovery.services_mutex);
    
    /* Seed random for announce intervals */
    srand((unsigned int)time(NULL));
    
    g_discovery.initialized = true;
    printf("pn_discovery: initialized on port %d, local IP %s\n", 
           g_discovery.udp_port, g_discovery.local_ip);
    
    return 0;
}

/* Build "helo" JSON message */
static int build_helo_message(char *buf, int maxlen) {
    int pos = 0;
    buf[pos++] = '{';
    
    pos = json_add_string(buf, pos, maxlen, "m", PN_MAGIC, false);
    if (pos < 0) return -1;
    
    pos = json_add_int(buf, pos, maxlen, "v", PN_VERSION, true);
    if (pos < 0) return -1;
    
    pos = json_add_string(buf, pos, maxlen, "cmd", "helo", true);
    if (pos < 0) return -1;
    
    pos = json_add_string(buf, pos, maxlen, "id", g_discovery.my_service.id, true);
    if (pos < 0) return -1;
    
    pos = json_add_string(buf, pos, maxlen, "svc", g_discovery.my_service.service, true);
    if (pos < 0) return -1;
    
    pos = json_add_string(buf, pos, maxlen, "ip", g_discovery.local_ip, true);
    if (pos < 0) return -1;
    
    pos = json_add_int(buf, pos, maxlen, "port", g_discovery.my_service.ctrl_port, true);
    if (pos < 0) return -1;
    
    if (g_discovery.my_service.data_port > 0) {
        pos = json_add_int(buf, pos, maxlen, "data", g_discovery.my_service.data_port, true);
        if (pos < 0) return -1;
    }
    
    if (g_discovery.my_service.caps[0]) {
        pos = json_add_string(buf, pos, maxlen, "caps", g_discovery.my_service.caps, true);
        if (pos < 0) return -1;
    }
    
    pos = json_add_int(buf, pos, maxlen, "ts", (int)time(NULL), true);
    if (pos < 0) return -1;
    
    if (pos + 1 >= maxlen) return -1;
    buf[pos++] = '}';
    buf[pos] = '\0';
    
    return pos;
}

/* Build "bye" JSON message */
static int build_bye_message(char *buf, int maxlen) {
    int pos = 0;
    buf[pos++] = '{';
    
    pos = json_add_string(buf, pos, maxlen, "m", PN_MAGIC, false);
    if (pos < 0) return -1;
    
    pos = json_add_int(buf, pos, maxlen, "v", PN_VERSION, true);
    if (pos < 0) return -1;
    
    pos = json_add_string(buf, pos, maxlen, "cmd", "bye", true);
    if (pos < 0) return -1;
    
    pos = json_add_string(buf, pos, maxlen, "id", g_discovery.my_service.id, true);
    if (pos < 0) return -1;
    
    pos = json_add_int(buf, pos, maxlen, "ts", (int)time(NULL), true);
    if (pos < 0) return -1;
    
    if (pos + 1 >= maxlen) return -1;
    buf[pos++] = '}';
    buf[pos] = '\0';
    
    return pos;
}

/* Broadcast message to all interfaces */
static void broadcast_message(const char *msg, int len) {
#ifdef _WIN32
    /* Windows: Get adapter addresses and broadcast on each */
    ULONG buflen = 15000;
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)malloc(buflen);
    if (!addrs) return;
    
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &buflen) == ERROR_SUCCESS) {
        PIP_ADAPTER_ADDRESSES adapter = addrs;
        while (adapter) {
            if (adapter->OperStatus == IfOperStatusUp) {
                PIP_ADAPTER_UNICAST_ADDRESS unicast = adapter->FirstUnicastAddress;
                while (unicast) {
                    struct sockaddr_in *sin = (struct sockaddr_in*)unicast->Address.lpSockaddr;
                    if (sin->sin_family == AF_INET) {
                        /* Calculate broadcast address */
                        ULONG mask = 0xFFFFFFFF << (32 - unicast->OnLinkPrefixLength);
                        ULONG ip = ntohl(sin->sin_addr.s_addr);
                        ULONG broadcast_ip = ip | ~mask;
                        
                        struct sockaddr_in dest;
                        memset(&dest, 0, sizeof(dest));
                        dest.sin_family = AF_INET;
                        dest.sin_port = htons(g_discovery.udp_port);
                        dest.sin_addr.s_addr = htonl(broadcast_ip);
                        
                        sendto(g_discovery.sock, msg, len, 0,
                               (struct sockaddr*)&dest, sizeof(dest));
                    }
                    unicast = unicast->Next;
                }
            }
            adapter = adapter->Next;
        }
    }
    free(addrs);
    
    /* Also send to 255.255.255.255 */
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(g_discovery.udp_port);
    dest.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(g_discovery.sock, msg, len, 0, (struct sockaddr*)&dest, sizeof(dest));
    
#else
    /* Linux: Use getifaddrs */
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        /* Fallback to 255.255.255.255 */
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(g_discovery.udp_port);
        dest.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(g_discovery.sock, msg, len, 0, (struct sockaddr*)&dest, sizeof(dest));
        return;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_BROADCAST)) continue;
        if (ifa->ifa_broadaddr == NULL) continue;
        
        struct sockaddr_in *bcast = (struct sockaddr_in*)ifa->ifa_broadaddr;
        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(g_discovery.udp_port);
        dest.sin_addr = bcast->sin_addr;
        
        sendto(g_discovery.sock, msg, len, 0, (struct sockaddr*)&dest, sizeof(dest));
    }
    
    freeifaddrs(ifaddr);
#endif
}

/* Parse incoming message */
static int parse_message(const char *buf, int len, const char *sender_ip) {
    char magic[8], cmd[16], id[PN_MAX_ID_LEN], svc[PN_MAX_SERVICE_LEN];
    char ip[PN_MAX_IP_LEN], caps[PN_MAX_CAPS_LEN];
    
    /* Verify magic */
    if (!json_get_string(buf, "m", magic, sizeof(magic))) return -1;
    if (strcmp(magic, PN_MAGIC) != 0) return -1;
    
    /* Get command */
    if (!json_get_string(buf, "cmd", cmd, sizeof(cmd))) return -1;
    
    /* Get ID */
    if (!json_get_string(buf, "id", id, sizeof(id))) return -1;
    
    /* Ignore our own messages */
    if (g_discovery.announcing && strcmp(id, g_discovery.my_service.id) == 0) {
        return 0;
    }
    
    if (strcmp(cmd, "helo") == 0) {
        /* Parse service info */
        if (!json_get_string(buf, "svc", svc, sizeof(svc))) return -1;
        
        /* Get IP - use sender_ip if not in message */
        if (!json_get_string(buf, "ip", ip, sizeof(ip))) {
            strncpy(ip, sender_ip, sizeof(ip) - 1);
        }
        
        int port = json_get_int(buf, "port");
        int data_port = json_get_int(buf, "data");
        
        json_get_string(buf, "caps", caps, sizeof(caps));
        if (!caps[0]) caps[0] = '\0';
        
        /* Update registry */
        mutex_lock(&g_discovery.services_mutex);
        
        int idx = -1;
        bool is_new = false;
        
        /* Check if we already know this service */
        for (int i = 0; i < PN_MAX_SERVICES; i++) {
            if (g_discovery.services[i].active && 
                strcmp(g_discovery.services[i].id, id) == 0) {
                idx = i;
                break;
            }
        }
        
        if (idx < 0) {
            /* New service - find empty slot */
            is_new = true;
            for (int i = 0; i < PN_MAX_SERVICES; i++) {
                if (!g_discovery.services[i].active) {
                    idx = i;
                    break;
                }
            }
        }
        
        if (idx >= 0) {
            pn_service_t *s = &g_discovery.services[idx];
            strncpy(s->id, id, PN_MAX_ID_LEN - 1);
            strncpy(s->service, svc, PN_MAX_SERVICE_LEN - 1);
            strncpy(s->ip, ip, PN_MAX_IP_LEN - 1);
            s->ctrl_port = port;
            s->data_port = data_port;
            strncpy(s->caps, caps, PN_MAX_CAPS_LEN - 1);
            s->last_seen = (uint32_t)time(NULL);
            s->active = true;
        }
        
        mutex_unlock(&g_discovery.services_mutex);
        
        /* Only callback and log for NEW services */
        if (is_new) {
            if (g_discovery.callback) {
                g_discovery.callback(id, svc, ip, port, data_port, caps, false,
                                    g_discovery.callback_userdata);
            }
            printf("pn_discovery: found %s '%s' at %s:%d\n", svc, id, ip, port);
        }
        
    } else if (strcmp(cmd, "bye") == 0) {
        /* Remove from registry */
        mutex_lock(&g_discovery.services_mutex);
        
        char svc_copy[PN_MAX_SERVICE_LEN] = "";
        char ip_copy[PN_MAX_IP_LEN] = "";
        int port_copy = 0;
        
        for (int i = 0; i < PN_MAX_SERVICES; i++) {
            if (g_discovery.services[i].active && 
                strcmp(g_discovery.services[i].id, id) == 0) {
                strncpy(svc_copy, g_discovery.services[i].service, PN_MAX_SERVICE_LEN - 1);
                strncpy(ip_copy, g_discovery.services[i].ip, PN_MAX_IP_LEN - 1);
                port_copy = g_discovery.services[i].ctrl_port;
                g_discovery.services[i].active = false;
                break;
            }
        }
        
        mutex_unlock(&g_discovery.services_mutex);
        
        /* Callback */
        if (g_discovery.callback && svc_copy[0]) {
            g_discovery.callback(id, svc_copy, ip_copy, port_copy, 0, "", true,
                                g_discovery.callback_userdata);
        }
        
        printf("pn_discovery: '%s' left the network\n", id);
    }
    
    return 0;
}

/* Get random announce interval */
static int get_random_interval(void) {
    int range = PN_ANNOUNCE_MAX_SEC - PN_ANNOUNCE_MIN_SEC;
    return PN_ANNOUNCE_MIN_SEC + (rand() % (range + 1));
}

/* Announce thread */
#ifdef _WIN32
static DWORD WINAPI announce_thread_func(LPVOID param) {
#else
static void* announce_thread_func(void *param) {
#endif
    (void)param;
    
    char msg[PN_MAX_MSG_LEN];
    
    /* Initial announcement */
    int len = build_helo_message(msg, sizeof(msg));
    if (len > 0) {
        broadcast_message(msg, len);
    }
    
    while (g_discovery.announce_running) {
        int interval = get_random_interval();
        
        /* Sleep in 1-second increments so we can stop quickly */
        for (int i = 0; i < interval && g_discovery.announce_running; i++) {
            sleep_ms(1000);
        }
        
        if (!g_discovery.announce_running) break;
        
        len = build_helo_message(msg, sizeof(msg));
        if (len > 0) {
            broadcast_message(msg, len);
        }
    }
    
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Listen thread */
#ifdef _WIN32
static DWORD WINAPI listen_thread_func(LPVOID param) {
#else
static void* listen_thread_func(void *param) {
#endif
    (void)param;
    
    char buf[PN_MAX_MSG_LEN];
    struct sockaddr_in sender;
    socklen_t sender_len;
    
    /* Set receive timeout */
#ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(g_discovery.sock, SOL_SOCKET, SO_RCVTIMEO, 
               (const char*)&timeout, sizeof(timeout));
#else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_discovery.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    
    while (g_discovery.listen_running) {
        sender_len = sizeof(sender);
        int len = recvfrom(g_discovery.sock, buf, sizeof(buf) - 1, 0,
                          (struct sockaddr*)&sender, &sender_len);
        
        if (len > 0) {
            buf[len] = '\0';
            char sender_ip[64];
            inet_ntop(AF_INET, &sender.sin_addr, sender_ip, sizeof(sender_ip));
            parse_message(buf, len, sender_ip);
        }
    }
    
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Start announcing */
int pn_announce(const char *id, const char *service,
                int ctrl_port, int data_port, const char *caps) {
    if (!g_discovery.initialized) {
        fprintf(stderr, "pn_discovery: not initialized\n");
        return -1;
    }
    
    if (g_discovery.announcing) {
        pn_announce_stop();
    }
    
    /* Store service info */
    strncpy(g_discovery.my_service.id, id, PN_MAX_ID_LEN - 1);
    strncpy(g_discovery.my_service.service, service, PN_MAX_SERVICE_LEN - 1);
    strncpy(g_discovery.my_service.ip, g_discovery.local_ip, PN_MAX_IP_LEN - 1);
    g_discovery.my_service.ctrl_port = ctrl_port;
    g_discovery.my_service.data_port = data_port;
    if (caps) {
        strncpy(g_discovery.my_service.caps, caps, PN_MAX_CAPS_LEN - 1);
    }
    
    /* Start announce thread */
    g_discovery.announce_running = true;
    g_discovery.announcing = true;
    
#ifdef _WIN32
    g_discovery.announce_thread = CreateThread(NULL, 0, announce_thread_func, NULL, 0, NULL);
    if (g_discovery.announce_thread == NULL) {
        g_discovery.announcing = false;
        g_discovery.announce_running = false;
        return -1;
    }
#else
    if (pthread_create(&g_discovery.announce_thread, NULL, announce_thread_func, NULL) != 0) {
        g_discovery.announcing = false;
        g_discovery.announce_running = false;
        return -1;
    }
#endif
    
    printf("pn_discovery: announcing as %s '%s' on port %d\n", service, id, ctrl_port);
    return 0;
}

/* Stop announcing */
void pn_announce_stop(void) {
    if (!g_discovery.announcing) return;
    
    /* Send bye message */
    char msg[PN_MAX_MSG_LEN];
    int len = build_bye_message(msg, sizeof(msg));
    if (len > 0) {
        broadcast_message(msg, len);
    }
    
    /* Stop thread */
    g_discovery.announce_running = false;
    
#ifdef _WIN32
    WaitForSingleObject(g_discovery.announce_thread, 5000);
    CloseHandle(g_discovery.announce_thread);
#else
    pthread_join(g_discovery.announce_thread, NULL);
#endif
    
    g_discovery.announcing = false;
    printf("pn_discovery: stopped announcing\n");
}

/* Start listening */
int pn_listen(pn_service_cb callback, void *userdata) {
    if (!g_discovery.initialized) {
        fprintf(stderr, "pn_discovery: not initialized\n");
        return -1;
    }
    
    if (g_discovery.listening) {
        return 0;  /* Already listening */
    }
    
    g_discovery.callback = callback;
    g_discovery.callback_userdata = userdata;
    g_discovery.listen_running = true;
    g_discovery.listening = true;
    
#ifdef _WIN32
    g_discovery.listen_thread = CreateThread(NULL, 0, listen_thread_func, NULL, 0, NULL);
    if (g_discovery.listen_thread == NULL) {
        g_discovery.listening = false;
        g_discovery.listen_running = false;
        return -1;
    }
#else
    if (pthread_create(&g_discovery.listen_thread, NULL, listen_thread_func, NULL) != 0) {
        g_discovery.listening = false;
        g_discovery.listen_running = false;
        return -1;
    }
#endif
    
    printf("pn_discovery: listening for services\n");
    return 0;
}

/* Find service by type */
const pn_service_t* pn_find_service(const char *service_type) {
    mutex_lock(&g_discovery.services_mutex);
    
    for (int i = 0; i < PN_MAX_SERVICES; i++) {
        if (g_discovery.services[i].active &&
            strcmp(g_discovery.services[i].service, service_type) == 0) {
            mutex_unlock(&g_discovery.services_mutex);
            return &g_discovery.services[i];
        }
    }
    
    mutex_unlock(&g_discovery.services_mutex);
    return NULL;
}

/* Find service by ID */
const pn_service_t* pn_find_service_by_id(const char *id) {
    mutex_lock(&g_discovery.services_mutex);
    
    for (int i = 0; i < PN_MAX_SERVICES; i++) {
        if (g_discovery.services[i].active &&
            strcmp(g_discovery.services[i].id, id) == 0) {
            mutex_unlock(&g_discovery.services_mutex);
            return &g_discovery.services[i];
        }
    }
    
    mutex_unlock(&g_discovery.services_mutex);
    return NULL;
}

/* Get all services */
int pn_get_services(pn_service_t *out, int max_count) {
    int count = 0;
    
    mutex_lock(&g_discovery.services_mutex);
    
    for (int i = 0; i < PN_MAX_SERVICES && count < max_count; i++) {
        if (g_discovery.services[i].active) {
            memcpy(&out[count], &g_discovery.services[i], sizeof(pn_service_t));
            count++;
        }
    }
    
    mutex_unlock(&g_discovery.services_mutex);
    return count;
}

/* Get service count */
int pn_get_service_count(void) {
    int count = 0;
    
    mutex_lock(&g_discovery.services_mutex);
    
    for (int i = 0; i < PN_MAX_SERVICES; i++) {
        if (g_discovery.services[i].active) {
            count++;
        }
    }
    
    mutex_unlock(&g_discovery.services_mutex);
    return count;
}

/* Shutdown */
void pn_discovery_shutdown(void) {
    if (!g_discovery.initialized) return;
    
    /* Stop announcing */
    if (g_discovery.announcing) {
        pn_announce_stop();
    }
    
    /* Stop listening */
    if (g_discovery.listening) {
        g_discovery.listen_running = false;
#ifdef _WIN32
        WaitForSingleObject(g_discovery.listen_thread, 5000);
        CloseHandle(g_discovery.listen_thread);
#else
        pthread_join(g_discovery.listen_thread, NULL);
#endif
        g_discovery.listening = false;
    }
    
    /* Close socket */
    if (g_discovery.sock != INVALID_SOCK) {
        close_socket(g_discovery.sock);
        g_discovery.sock = INVALID_SOCK;
    }
    
    /* Destroy mutex */
    mutex_destroy(&g_discovery.services_mutex);
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    g_discovery.initialized = false;
    printf("pn_discovery: shutdown complete\n");
}

/* Get local IP address */
int pn_get_local_ip(char *out, int maxlen) {
#ifdef _WIN32
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strncpy(out, "127.0.0.1", maxlen - 1);
        return -1;
    }
    
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        strncpy(out, "127.0.0.1", maxlen - 1);
        return -1;
    }
    
    struct sockaddr_in *addr = (struct sockaddr_in*)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, out, maxlen);
    freeaddrinfo(res);
    
#else
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        strncpy(out, "127.0.0.1", maxlen - 1);
        return -1;
    }
    
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        
        struct sockaddr_in *addr = (struct sockaddr_in*)ifa->ifa_addr;
        if (addr->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) continue;
        
        inet_ntop(AF_INET, &addr->sin_addr, out, maxlen);
        freeifaddrs(ifaddr);
        return 0;
    }
    
    freeifaddrs(ifaddr);
    strncpy(out, "127.0.0.1", maxlen - 1);
#endif
    
    return 0;
}
