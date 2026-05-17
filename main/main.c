/*
 * MinerBridge -- WT32-ETH01 WiFi-to-Ethernet NAT bridge for Bitcoin miners
 *
 * First boot  : Creates "MinerBridge" WiFi AP, connect, open http://192.168.4.1
 *               Enter home WiFi SSID + password, Save, reboot.
 *
 * Normal boot : Connects to saved WiFi as STA. Ethernet port runs DHCP server
 *               (192.168.4.x). Miner plugs in, gets internet via NAT.
 *               Port 8080 on the WT32 WiFi IP forwards to the miner UI on port 80.
 *
 * Hardware    : WT32-ETH01 (ESP32 + LAN8720)
 * IDF version : v5.x
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_eth_com.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_http_server.h"
#include "esp_mac.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "lwip/inet.h"
#include "lwip/lwip_napt.h"

static const char *TAG = "MinerBridge";

/* WT32-ETH01 pins */
#define ETH_PHY_ADDR      1
#define ETH_MDC_GPIO      23
#define ETH_MDIO_GPIO     18
#define ETH_PHY_RST_GPIO  (-1)
#define ETH_PWR_GPIO      16
#define LED_GPIO          2

/* Network */
#define SETUP_AP_SSID   "MinerBridge"
#define SETUP_AP_PASS   ""
#define SETUP_IP        "192.168.4.1"
#define ETH_STATIC_IP   "192.168.4.1"
#define ETH_NETMASK     "255.255.255.0"
#define DNS_SERVER      "8.8.8.8"

/* Port forwarding: hitting WT32_WIFI_IP:FORWARD_PORT opens miner UI */
#define MINER_UI_PORT   80
#define FORWARD_PORT    8080

/* NVS */
#define NVS_NS   "mb"
#define NVS_SSID "ssid"
#define NVS_PASS "pass"

/* Global state */
static char g_ssid[33]     = {0};
static char g_pass[65]     = {0};
static bool g_has_creds    = false;
static bool g_wifi_up      = false;
static char g_sta_ip[16]   = "Connecting...";
static char g_miner_ip[16] = "Not connected";
static bool g_miner_online = false;

static esp_netif_t    *g_ap_netif  = NULL;
static esp_netif_t    *g_sta_netif = NULL;
static esp_netif_t    *g_eth_netif = NULL;
static httpd_handle_t  g_httpd     = NULL;

/* -------------------------------------------------------------------------
 * HTML pages
 * ---------------------------------------------------------------------- */

static const char SETUP_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MinerBridge Setup</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0d0d0d;color:#e0e0e0;font-family:'Segoe UI',Arial,sans-serif;"
"display:flex;justify-content:center;align-items:center;min-height:100vh;padding:16px}"
".card{background:#161616;border:1px solid #252525;border-radius:16px;"
"padding:36px 32px;width:100%;max-width:400px}"
"h1{font-size:1.45rem;color:#f5a623;margin-bottom:4px;font-weight:700}"
".sub{color:#666;font-size:.88rem;margin-bottom:28px;margin-top:6px;line-height:1.5}"
"label{display:block;color:#888;font-size:.75rem;text-transform:uppercase;"
"letter-spacing:.08em;margin-bottom:7px;margin-top:18px}"
"input{width:100%;background:#111;border:1px solid #2a2a2a;border-radius:8px;"
"padding:13px 14px;color:#e0e0e0;font-size:.97rem}"
"input:focus{outline:none;border-color:#f5a623}"
"button{width:100%;margin-top:26px;background:#f5a623;color:#000;border:none;"
"border-radius:8px;padding:14px;font-size:1rem;font-weight:700;cursor:pointer}"
".note{color:#444;font-size:.78rem;text-align:center;margin-top:14px}"
"</style></head>"
"<body><div class='card'>"
"<h1>&#9935; MinerBridge</h1>"
"<p class='sub'>Connect your miner to WiFi via Ethernet.<br>Enter your home WiFi details below.</p>"
"<form action='/save' method='POST'>"
"<label>WiFi Network (SSID)</label>"
"<input type='text' name='ssid' placeholder='Your network name' required>"
"<label>WiFi Password</label>"
"<input type='password' name='pass' placeholder='Leave blank if open network'>"
"<button type='submit'>Save &amp; Connect</button>"
"</form>"
"<p class='note'>Device will reboot and your miner will be online.</p>"
"</div></body></html>";

static const char SAVED_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>MinerBridge</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0d0d0d;color:#e0e0e0;font-family:'Segoe UI',Arial,sans-serif;"
"display:flex;justify-content:center;align-items:center;min-height:100vh;padding:16px}"
".card{background:#161616;border:1px solid #252525;border-radius:16px;"
"padding:36px 32px;width:100%;max-width:400px;text-align:center}"
"h1{font-size:1.45rem;color:#f5a623;margin-bottom:20px}"
".ok{color:#4caf50;font-size:1.05rem;margin-bottom:16px;font-weight:600}"
".hint{color:#666;font-size:.88rem;line-height:1.7}"
"</style></head>"
"<body><div class='card'>"
"<h1>&#9935; MinerBridge</h1>"
"<p class='ok'>&#10003; Settings saved!</p>"
"<p class='hint'>Rebooting now...<br><br>"
"Plug your miner into the Ethernet port.<br>"
"It will connect automatically.</p>"
"</div></body></html>";

/* Status page -- 6 string args + 1 int:
   ssid, dot-class, status-text, sta_ip, miner_ip, miner_link_html, forward_port */
#define STATUS_HTML_FMT \
"<!DOCTYPE html><html><head>" \
"<meta name='viewport' content='width=device-width,initial-scale=1'>" \
"<meta http-equiv='refresh' content='10'>" \
"<title>MinerBridge</title>" \
"<style>" \
"*{box-sizing:border-box;margin:0;padding:0}" \
"body{background:#0d0d0d;color:#e0e0e0;font-family:'Segoe UI',Arial,sans-serif;" \
"display:flex;justify-content:center;align-items:center;min-height:100vh;padding:16px}" \
".card{background:#161616;border:1px solid #252525;border-radius:16px;" \
"padding:36px 32px;width:100%%;max-width:440px}" \
"h1{font-size:1.45rem;color:#f5a623;margin-bottom:6px;font-weight:700}" \
".sub{color:#333;font-size:.72rem;margin-bottom:24px}" \
".row{display:flex;justify-content:space-between;align-items:center;" \
"padding:12px 0;border-bottom:1px solid #1e1e1e}" \
".lbl{color:#555;font-size:.83rem}" \
".val{color:#ccc;font-size:.88rem;font-weight:500;text-align:right;max-width:60%%}" \
".dot{width:8px;height:8px;border-radius:50%%;display:inline-block;margin-right:5px}" \
".green{background:#4caf50}" \
".orange{background:#ff9800}" \
".vrow{display:flex;align-items:center;justify-content:flex-end}" \
".miner-btn{display:block;width:100%%;margin-top:20px;background:#f5a623;color:#000;" \
"border:none;border-radius:8px;padding:13px;font-size:.95rem;font-weight:700;" \
"text-align:center;text-decoration:none;cursor:pointer}" \
".miner-off{display:block;width:100%%;margin-top:20px;background:#1a1a1a;color:#444;" \
"border:1px solid #252525;border-radius:8px;padding:13px;font-size:.88rem;" \
"text-align:center;cursor:default}" \
".actions{margin-top:10px;display:flex;gap:10px}" \
".btn{flex:1;background:#1a1a1a;color:#666;border:1px solid #2a2a2a;" \
"border-radius:8px;padding:10px 8px;font-size:.8rem;" \
"text-align:center;text-decoration:none}" \
".btn-danger{color:#c0392b;border-color:#2a1515}" \
"</style></head><body>" \
"<div class='card'>" \
"<h1>&#9935; MinerBridge</h1>" \
"<p class='sub'>Auto-refreshes every 10s</p>" \
"<div class='row'><span class='lbl'>WiFi Network</span><span class='val'>%s</span></div>" \
"<div class='row'><span class='lbl'>WiFi Status</span>" \
"<span class='val'><span class='vrow'><span class='dot %s'></span>%s</span></span></div>" \
"<div class='row'><span class='lbl'>WiFi IP</span><span class='val'>%s</span></div>" \
"<div class='row'><span class='lbl'>Miner IP</span><span class='val'>%s</span></div>" \
"<div class='row'><span class='lbl'>Miner UI port</span><span class='val'>%d</span></div>" \
"%s" \
"<div class='actions'>" \
"<a href='/' class='btn'>&#8635; Refresh</a>" \
"<a href='/reset' class='btn btn-danger' " \
"onclick=\"return confirm('Reset WiFi settings?')\">&#9881; Reconfigure</a>" \
"</div></div></body></html>"

/* -------------------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------------- */

static void url_decode(char *dst, const char *src, size_t max_len)
{
    size_t i = 0, j = 0;
    while (src[i] && j < max_len - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static bool get_param(const char *body, const char *key, char *out, size_t max_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(body, search);
    if (!p) { out[0] = '\0'; return false; }
    p += strlen(search);
    const char *end = strchr(p, '&');
    size_t raw_len = end ? (size_t)(end - p) : strlen(p);
    char encoded[256] = {0};
    if (raw_len >= sizeof(encoded)) raw_len = sizeof(encoded) - 1;
    memcpy(encoded, p, raw_len);
    url_decode(out, encoded, max_len);
    return true;
}

/* -------------------------------------------------------------------------
 * NVS
 * ---------------------------------------------------------------------- */

static void nvs_load_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(g_ssid);
    if (nvs_get_str(h, NVS_SSID, g_ssid, &len) == ESP_OK && strlen(g_ssid) > 0)
        g_has_creds = true;
    len = sizeof(g_pass);
    nvs_get_str(h, NVS_PASS, g_pass, &len);
    nvs_close(h);
    if (g_has_creds) ESP_LOGI(TAG, "Credentials loaded: %s", g_ssid);
    else ESP_LOGI(TAG, "No credentials saved");
}

static void nvs_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Credentials saved: %s", ssid);
}

static void nvs_clear_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_SSID);
    nvs_erase_key(h, NVS_PASS);
    nvs_commit(h);
    nvs_close(h);
}

/* -------------------------------------------------------------------------
 * Port forwarding helpers
 * ---------------------------------------------------------------------- */

static void setup_port_forward(void)
{
    if (!g_miner_online || !g_wifi_up) return;

    /* ip_portmap_add(proto, wan_ip, wan_port, lan_ip, lan_port)
     * proto 6 = TCP. Returns 1 on success. */
    uint32_t miner_addr = inet_addr(g_miner_ip);
    uint32_t sta_addr   = inet_addr(g_sta_ip);

    if (ip_portmap_add(6, sta_addr, FORWARD_PORT, miner_addr, MINER_UI_PORT)) {
        ESP_LOGI(TAG, "Port forward OK: WiFi:%d -> %s:%d", FORWARD_PORT, g_miner_ip, MINER_UI_PORT);
    } else {
        ESP_LOGW(TAG, "Port forward failed -- miner UI on Ethernet side only");
    }
}

/* -------------------------------------------------------------------------
 * HTTP handlers
 * ---------------------------------------------------------------------- */

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    if (!g_has_creds) {
        httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Build miner button */
    char miner_btn[256] = {0};
    if (g_miner_online && g_wifi_up) {
        snprintf(miner_btn, sizeof(miner_btn),
            "<a class='miner-btn' href='http://%s:%d' target='_blank'>"
            "&#9881; Open Miner UI</a>",
            g_sta_ip, FORWARD_PORT);
    } else {
        snprintf(miner_btn, sizeof(miner_btn),
            "<div class='miner-off'>Miner UI &mdash; waiting for miner...</div>");
    }

    static char html[4096];
    const char *dot  = g_wifi_up ? "green"     : "orange";
    const char *stat = g_wifi_up ? "Connected" : "Connecting...";
    snprintf(html, sizeof(html), STATUS_HTML_FMT,
             g_ssid, dot, stat, g_sta_ip, g_miner_ip, FORWARD_PORT, miner_btn);
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_save(httpd_req_t *req)
{
    char body[384] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    char ssid[33] = {0}, pass[65] = {0};
    get_param(body, "ssid", ssid, sizeof(ssid));
    get_param(body, "pass", pass, sizeof(pass));

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID cannot be empty");
        return ESP_FAIL;
    }

    nvs_save_creds(ssid, pass);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t handler_reset(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<style>"
        "body{background:#0d0d0d;color:#e0e0e0;font-family:sans-serif;"
        "display:flex;justify-content:center;align-items:center;height:100vh}"
        ".c{background:#161616;border:1px solid #252525;border-radius:16px;"
        "padding:36px;text-align:center;max-width:360px}"
        "h2{color:#f5a623;margin-bottom:12px}p{color:#666;line-height:1.6}"
        "</style></head><body>"
        "<div class='c'><h2>&#9935; MinerBridge</h2>"
        "<p>Clearing settings and rebooting...<br><br>"
        "Reconnect to <strong>MinerBridge</strong> WiFi.</p>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    nvs_clear_creds();
    esp_restart();
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config    = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable  = true;
    config.max_uri_handlers  = 8;
    config.stack_size        = 8192;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",      .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/save",  .method = HTTP_POST, .handler = handler_save },
        { .uri = "/reset", .method = HTTP_GET,  .handler = handler_reset },
    };
    for (int i = 0; i < 3; i++)
        httpd_register_uri_handler(server, &uris[i]);

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

/* -------------------------------------------------------------------------
 * Ethernet init (WT32-ETH01 / LAN8720)
 * ---------------------------------------------------------------------- */

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        /* Physical link up — miner has plugged in, first DHCP lease = .2 */
        snprintf(g_miner_ip, sizeof(g_miner_ip), "192.168.4.2");
        g_miner_online = true;
        ESP_LOGI(TAG, "Ethernet link up — miner assumed at %s", g_miner_ip);
        setup_port_forward();
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        g_miner_online = false;
        snprintf(g_miner_ip, sizeof(g_miner_ip), "Not connected");
        ESP_LOGW(TAG, "Ethernet link down — miner disconnected");
    }
}

static void eth_init(void)
{
    /* Power LAN8720 */
    gpio_config_t pwr = {
        .pin_bit_mask = (1ULL << ETH_PWR_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(ETH_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = ETH_MDIO_GPIO;
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth_handle));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    g_eth_netif = esp_netif_new(&netif_cfg);

    /* Static IP, DHCP server for miner */
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(g_eth_netif));
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = inet_addr(ETH_STATIC_IP);
    ip.gw.addr      = inet_addr(ETH_STATIC_IP);
    ip.netmask.addr = inet_addr(ETH_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_eth_netif, &ip));

    esp_netif_dns_info_t dns = {0};
    dns.ip.u_addr.ip4.addr = inet_addr(DNS_SERVER);
    dns.ip.type            = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(g_eth_netif, ESP_NETIF_DNS_MAIN, &dns);

    ESP_ERROR_CHECK(esp_netif_dhcps_start(g_eth_netif));

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(g_eth_netif, glue));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    /* Listen for Ethernet link events (miner plug/unplug) */
    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));

    ESP_LOGI(TAG, "Ethernet up -- gateway %s, DHCP server running", ETH_STATIC_IP);
}

/* -------------------------------------------------------------------------
 * WiFi event handler
 * ---------------------------------------------------------------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_up = false;
        snprintf(g_sta_ip, sizeof(g_sta_ip), "Disconnected");
        gpio_set_level(LED_GPIO, 0);
        ESP_LOGW(TAG, "WiFi disconnected -- retrying in 5s");
        vTaskDelay(pdMS_TO_TICKS(5000));
        esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(g_sta_ip, sizeof(g_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        g_wifi_up = true;
        gpio_set_level(LED_GPIO, 1);
        ESP_LOGI(TAG, "WiFi UP -- STA IP: %s", g_sta_ip);

        ip_napt_enable(inet_addr(ETH_STATIC_IP), 1);
        ESP_LOGI(TAG, "NAPT enabled");

        /* If miner was already connected before WiFi came up, set forwarding now */
        if (g_miner_online) setup_port_forward();
    }
}

/* -------------------------------------------------------------------------
 * Mode A: Setup AP
 * ---------------------------------------------------------------------- */

static void start_setup_mode(void)
{
    ESP_LOGI(TAG, "=== SETUP MODE -- AP: %s ===", SETUP_AP_SSID);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_ap_netif = esp_netif_create_default_wifi_ap();

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(g_ap_netif));
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr      = inet_addr(SETUP_IP);
    ip.gw.addr      = inet_addr(SETUP_IP);
    ip.netmask.addr = inet_addr("255.255.255.0");
    ESP_ERROR_CHECK(esp_netif_set_ip_info(g_ap_netif, &ip));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(g_ap_netif));

    wifi_config_t cfg = {
        .ap = {
            .ssid           = SETUP_AP_SSID,
            .ssid_len       = strlen(SETUP_AP_SSID),
            .password       = SETUP_AP_PASS,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
            .channel        = 1,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    g_httpd = start_webserver();
    ESP_LOGI(TAG, "Connect to '%s' then open http://%s", SETUP_AP_SSID, SETUP_IP);
}

/* -------------------------------------------------------------------------
 * Mode B: Router
 * ---------------------------------------------------------------------- */

static void start_router_mode(void)
{
    ESP_LOGI(TAG, "=== ROUTER MODE -- connecting to: %s ===", g_ssid);

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    g_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid,     g_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, g_pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode =
        (strlen(g_pass) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    eth_init();

    g_httpd = start_webserver();
    ESP_LOGI(TAG, "Status page: http://%s (from Ethernet side)", ETH_STATIC_IP);
    ESP_LOGI(TAG, "Miner UI will be: http://<WiFi-IP>:%d (from main network)", FORWARD_PORT);
}

/* -------------------------------------------------------------------------
 * LED
 * ---------------------------------------------------------------------- */

static void led_init(void)
{
    gpio_config_t cfg = { .pin_bit_mask = (1ULL << LED_GPIO), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, 0);
}

static void led_boot_blink(void)
{
    for (int i = 0; i < 4; i++) {
        gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(80));
    }
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "MinerBridge starting");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    led_init();
    led_boot_blink();

    nvs_load_creds();

    if (g_has_creds)
        start_router_mode();
    else
        start_setup_mode();
}
