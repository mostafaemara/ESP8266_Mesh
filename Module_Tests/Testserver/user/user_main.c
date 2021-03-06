// user_main.c
// Copyright 2017 Lukas Friedrichsen
// License: Modified BSD-License
//
// 2017-03-02

#include "mem.h"
#include "osapi.h"
#include "ets_sys.h"
#include "os_type.h"
#include "espconn.h"
#include "user_interface.h"
#include "user_config.h"

// Initialize local copies of SSID and PASSWD
// NO CONNECTION TO THE ACCESS-POINT WILL BE POSSIBLE OTHERWISE!
char ssid[32] = SSID;
char passwd[64] = PASSWD;

// Set broadcast ip
uint8_t broadcast_ip[4] = BROADCAST_IP;

os_timer_t *send_timer = NULL;

struct espconn* udp_socket = NULL;

static char msg_buffer[64]; // Buffer to store the message to send

// Callback-function that prints the message sent by the UDP-socket
static void ICACHE_FLASH_ATTR udp_sent_cb(void *arg) {
  os_printf("%d   Message sent!\n", system_get_time());
}

// Periodically broadcast a meta-data-request to all devices in the network
static void ICACHE_FLASH_ATTR send_timerfunc(void) {
  if (udp_socket) {
    uint8_t msg_len = os_sprintf(msg_buffer, REQUEST_STRING, os_strlen(REQUEST_STRING));
    os_memcpy(udp_socket->proto.udp->remote_ip, broadcast_ip, 4); // Has to be done before every call of espconn_send
    udp_socket->proto.udp->remote_port = DEVICE_COM_PORT;
    espconn_send(udp_socket, msg_buffer, msg_len);  // Broadcast the meta-data-request
  }
  else {
    os_printf("send_timerfunc: Please initialize udp_socket before arming the timer...\n");
  }
}

// Initialize UDP-socket
static void ICACHE_FLASH_ATTR udp_init(void) {
  // Set up UDP-socket-configuration
  udp_socket = (struct espconn *) os_zalloc(sizeof(struct espconn));
  if (!udp_socket) {
    os_printf("udp_init: Failed to initialize udp_socket!\n");
    if (send_timer) {
      os_free(send_timer);  // Free occupied resources
    }
    return;
  }
  udp_socket->type = ESPCONN_UDP;
  udp_socket->state = ESPCONN_NONE;
  udp_socket->proto.udp = (esp_udp *) os_zalloc(sizeof(esp_udp));
  if (!udp_socket->proto.udp) {
    os_printf("udp_init: Failed to initialize udp_socket->proto.udp!\n");
    // Free occupied resources
    os_free(udp_socket);
    if (send_timer) {
      os_free(send_timer);
    }
    return;
  }
  udp_socket->proto.udp->remote_port = DEVICE_COM_PORT;
  udp_socket->proto.udp->local_port = espconn_port();
  os_memcpy(udp_socket->proto.udp->remote_ip, broadcast_ip, 4);

  // Create UDP-socket and register sent-callback
  espconn_create(udp_socket);
  espconn_regist_sentcb(udp_socket, udp_sent_cb);

  // Start send_timer
  os_timer_arm(send_timer, REQUEST_INTERVAL, 1);
}

// Wifi-event-callback; print the current connection-status and initialize
// the UDP-socket on a successfully established connection
static void ICACHE_FLASH_ATTR wifi_event_cb(System_Event_t *evt) {
  os_printf("wifi_event_cb: WiFi-event: %x\n", evt->event);
  switch (evt->event) {
    case EVENT_SOFTAPMODE_STACONNECTED:
      os_printf("wifi_event_cb: Client connected!\n");
      /*if (!udp_socket && send_timer) {
        os_printf("wifi_event_cb: Starting UDP-broadcast!\n");
        udp_init(); // Initialize the UDP-socket and start the cyclic broadcast
      }*/
      break;
    default:
      break;
  }
}

// Initialize WiFi-softAp-mode
void ICACHE_FLASH_ATTR wifi_init(void) {
  // Set up softAP-configuration
  struct softap_config ap_conf;
  os_memcpy(&ap_conf.ssid, ssid, 32);
  os_memcpy(&ap_conf.password, passwd, 64);
  ap_conf.ssid_len = os_strlen(SSID);
  ap_conf.authmode = AUTH_WPA2_PSK;
  ap_conf.max_connection = 8;

  // Set softAP-mode and load softAP-configuration
  // Restart the system, if one of the above fails!
  if (!wifi_set_opmode(SOFTAP_MODE) || !wifi_softap_set_config_current(&ap_conf)) {
    os_printf("Error while initializing softAP-mode! Rebooting...\n");
    system_restart();
  }

  // Stop any running DHCP-server before setting up a new one
  wifi_softap_dhcps_stop();

  // Set up DHCP-configuration and start DHCP-server
  struct dhcps_lease dhcp_lease;
  IP4_ADDR(&dhcp_lease.start_ip, 192, 268, 4, 2);
  IP4_ADDR(&dhcp_lease.end_ip, 192, 168, 4, 127);
  wifi_softap_set_dhcps_lease(&dhcp_lease);
  wifi_softap_dhcps_start();

  // Allow broadcasts in softAP-mode
  wifi_set_broadcast_if(SOFTAP_MODE);

  // Set wifi-event-callback
  wifi_set_event_handler_cb(wifi_event_cb);
}

// Initializiation
void ICACHE_FLASH_ATTR user_init() {
    os_printf("user_init: Initializing...\n");

    // Initialize WiFi and UDP-socket
    wifi_init();

    os_printf("user_init: Setting callback for the timer...\n");

    // Initialize and set up timer
    send_timer = (os_timer_t *) os_zalloc(sizeof(os_timer_t));
    if (!send_timer) {
      os_printf("user_init: Failed to initialize send_timer!\n");
      return;
    }
    os_timer_disarm(send_timer);
    os_timer_setfn(send_timer, (os_timer_func_t *) send_timerfunc, NULL);
}

/******************************************************************************
 * FunctionName : user_rf_cal_sector_set
 * Description  : SDK just reversed 4 sectors, used for rf init data and paramters.
 *                We add this function to force users to set rf cal sector, since
 *                we don't know which sector is free in user's application.
 *                sector map for last several sectors : ABBBCDDD
 *                A : rf cal
 *                B : at parameters
 *                C : rf init data
 *                D : sdk parameters
 * Parameters   : none
 * Returns      : rf cal sector
*******************************************************************************/
uint32 ICACHE_FLASH_ATTR user_rf_cal_sector_set(void) {
    enum flash_size_map size_map = system_get_flash_size_map();
    uint32 rf_cal_sec = 0;

    switch (size_map) {
        case FLASH_SIZE_4M_MAP_256_256:
            rf_cal_sec = 128 - 8;
            break;

        case FLASH_SIZE_8M_MAP_512_512:
            rf_cal_sec = 256 - 5;
            break;

        case FLASH_SIZE_16M_MAP_512_512:
        case FLASH_SIZE_16M_MAP_1024_1024:
            rf_cal_sec = 512 - 5;
            break;

        case FLASH_SIZE_32M_MAP_512_512:
        case FLASH_SIZE_32M_MAP_1024_1024:
            rf_cal_sec = 1024 - 5;
            break;

        default:
            rf_cal_sec = 0;
            break;
    }

    return rf_cal_sec;
}

void ICACHE_FLASH_ATTR user_rf_pre_init(void) {
  // Nothing to do...
}
