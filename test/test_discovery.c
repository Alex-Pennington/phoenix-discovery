/*
 * Phoenix Nest Service Discovery - Test Program
 * 
 * Run two instances to test discovery:
 *   test_discovery server   (announces as sdr_server)
 *   test_discovery client   (announces as waterfall, looks for sdr_server)
 * 
 * (c) 2024 Phoenix Nest LLC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "pn_discovery.h"

#ifdef _WIN32
    #include <windows.h>
    #define sleep_sec(s) Sleep((s) * 1000)
#else
    #include <unistd.h>
    #define sleep_sec(s) sleep(s)
#endif

static volatile int running = 1;

void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

void on_service_found(const char *id, const char *service,
                      const char *ip, int ctrl_port, int data_port,
                      const char *caps, bool is_bye, void *userdata) {
    (void)userdata;
    
    if (is_bye) {
        printf("\n*** SERVICE LEFT: %s '%s'\n\n", service, id);
    } else {
        printf("\n*** SERVICE FOUND: %s '%s' at %s:%d", service, id, ip, ctrl_port);
        if (data_port > 0) printf(" data:%d", data_port);
        if (caps && caps[0]) printf(" caps:%s", caps);
        printf("\n\n");
    }
}

void print_usage(const char *prog) {
    printf("Usage: %s <mode> [id]\n", prog);
    printf("Modes:\n");
    printf("  server  - Announce as sdr_server\n");
    printf("  client  - Announce as waterfall, look for servers\n");
    printf("  listen  - Just listen, don't announce\n");
    printf("\n");
    printf("Example:\n");
    printf("  %s server KY4OLB-SDR1\n", prog);
    printf("  %s client WF1\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *mode = argv[1];
    const char *id = (argc > 2) ? argv[2] : "TEST1";
    
    /* Setup signal handler */
    signal(SIGINT, signal_handler);
#ifndef _WIN32
    signal(SIGTERM, signal_handler);
#endif
    
    /* Initialize */
    if (pn_discovery_init(0) < 0) {
        fprintf(stderr, "Failed to initialize discovery\n");
        return 1;
    }
    
    /* Start listening */
    if (pn_listen(on_service_found, NULL) < 0) {
        fprintf(stderr, "Failed to start listener\n");
        pn_discovery_shutdown();
        return 1;
    }
    
    /* Announce based on mode */
    if (strcmp(mode, "server") == 0) {
        if (pn_announce(id, PN_SVC_SDR_SERVER, 4535, 4536, "rsp2pro,2mhz") < 0) {
            fprintf(stderr, "Failed to start announcing\n");
            pn_discovery_shutdown();
            return 1;
        }
        printf("Announcing as sdr_server '%s' on ports 4535/4536\n", id);
        
    } else if (strcmp(mode, "client") == 0) {
        if (pn_announce(id, PN_SVC_WATERFALL, 0, 0, NULL) < 0) {
            fprintf(stderr, "Failed to start announcing\n");
            pn_discovery_shutdown();
            return 1;
        }
        printf("Announcing as waterfall '%s'\n", id);
        
    } else if (strcmp(mode, "listen") == 0) {
        printf("Listen-only mode\n");
        
    } else {
        fprintf(stderr, "Unknown mode: %s\n", mode);
        print_usage(argv[0]);
        pn_discovery_shutdown();
        return 1;
    }
    
    printf("Press Ctrl+C to exit...\n\n");
    
    /* Main loop */
    while (running) {
        sleep_sec(1);
        
        /* Periodically print discovered services */
        static int tick = 0;
        if (++tick >= 10) {
            tick = 0;
            int count = pn_get_service_count();
            if (count > 0) {
                printf("--- Known services (%d) ---\n", count);
                pn_service_t services[PN_MAX_SERVICES];
                int n = pn_get_services(services, PN_MAX_SERVICES);
                for (int i = 0; i < n; i++) {
                    printf("  %s '%s' at %s:%d\n", 
                           services[i].service, services[i].id,
                           services[i].ip, services[i].ctrl_port);
                }
                printf("---\n\n");
            }
        }
    }
    
    printf("\nShutting down...\n");
    pn_discovery_shutdown();
    
    return 0;
}
