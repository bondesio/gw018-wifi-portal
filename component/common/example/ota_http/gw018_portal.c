#include "gw018_portal.h"

#include <FreeRTOS.h>
#include <task.h>
#include <platform/platform_stdlib.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip_netconf.h>
#include <wifi_conf.h>
#include <dhcp/dhcps.h>
#include <wlan_fast_connect/example_wlan_fast_connect.h>
#include <rtl8721d_ota.h>

#define SETUP_SSID "GW018-Setup"
#define SETUP_IP "192.168.43.1"
#define SETUP_TIMEOUT_MS (10U * 60U * 1000U)
#define MAX_SCAN_RESULTS 12

static volatile int s_active;
static volatile int s_connecting;
static volatile int s_dns_running;
static volatile int s_dns_alive;
static volatile int s_http_alive;
static volatile int s_scan_done;
static volatile int s_scan_count;
static volatile int s_scan_failed;
static int s_scan_reported;
static TickType_t s_scan_started_at;
static TickType_t s_started_at;
static char s_scanned[MAX_SCAN_RESULTS][33];
static char s_pending_ssid[33];
static char s_pending_password[65];
static char s_request[1536];
static char s_page[3600];
static write_reconnect_ptr s_flash_writer;
static struct wlan_fast_reconnect s_trial_profile;
static int s_trial_profile_valid;
static int s_last_failed;
extern struct netif xnetif[NET_IF_NUM];
static void http_task(void *arg);

static int profile_capture(uint8_t *data, uint32_t len)
{
    if (len != sizeof(s_trial_profile) || data == NULL) return -1;
    memcpy(&s_trial_profile, data, len);
    s_trial_profile_valid = 1;
    return 0;
}

static rtw_result_t scan_result(rtw_scan_handler_result_t *result)
{
    if (result->scan_complete == RTW_TRUE) {
        s_scan_done = 1;
        return RTW_SUCCESS;
    }
    if (s_scan_count >= MAX_SCAN_RESULTS) return RTW_SUCCESS;
    const rtw_scan_result_t *ap = &result->ap_details;
    if (ap->SSID.len == 0 || ap->SSID.len > 32) return RTW_SUCCESS;
    char safe[33];
    int out = 0;
    for (int i = 0; i < ap->SSID.len; i++) {
        unsigned char c = ap->SSID.val[i];
        if (c < 0x20 || c > 0x7e || c == '\'' || c == '"' ||
            c == '<' || c == '>' || c == '&' || c == '\\') continue;
        safe[out++] = (char)c;
    }
    if (!out) return RTW_SUCCESS;
    safe[out] = '\0';
    for (int i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scanned[i], safe) == 0) return RTW_SUCCESS;
    }
    strcpy(s_scanned[s_scan_count++], safe);
    return RTW_SUCCESS;
}

static void start_scan(void)
{
    if (!s_active || !s_scan_done) return;
    s_scan_count = 0;
    s_scan_done = 0;
    s_scan_failed = 0;
    s_scan_reported = 0;
    s_scan_started_at = xTaskGetTickCount();
    int result = wifi_scan_networks(scan_result, NULL);
    if (result != RTW_SUCCESS) {
        s_scan_failed = 1;
        s_scan_done = 1;
    }
}

static void dns_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) goto done;
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = inet_addr(SETUP_IP);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) != 0) {
        printf("GW018 setup: DNS bind failed\n");
        close(fd);
        goto done;
    }
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    uint8_t packet[512];
    while (s_dns_running) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int n = recvfrom(fd, packet, sizeof(packet), 0,
                         (struct sockaddr *)&peer, &peer_len);
        if (n < 17 || packet[4] != 0 || packet[5] != 1) continue;
        int q = 12;
        while (q < n && packet[q] && packet[q] <= 63 && q + 1 + packet[q] < n)
            q += 1 + packet[q];
        if (q + 5 > n || packet[q] != 0) continue;
        q++;
        int is_a = packet[q] == 0 && packet[q + 1] == 1 &&
                   packet[q + 2] == 0 && packet[q + 3] == 1;
        int response_len = q + 4;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = is_a ? 1 : 0;
        memset(packet + 8, 0, 4);
        if (is_a && response_len + 16 <= sizeof(packet)) {
            static const uint8_t answer[] = {
                0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 43, 1
            };
            memcpy(packet + response_len, answer, sizeof(answer));
            response_len += sizeof(answer);
        }
        sendto(fd, packet, response_len, 0, (struct sockaddr *)&peer, peer_len);
    }
    close(fd);
done:
    s_dns_alive = 0;
    vTaskDelete(NULL);
}

static void portal_stop(void)
{
    if (!s_active) return;
    s_active = 0;
    s_dns_running = 0;
    if (wifi_set_mode(RTW_MODE_STA) != 0)
        printf("GW018 setup: could not leave concurrent Wi-Fi mode\n");
    printf("GW018 setup: AP stopped\n");
}

void gw018_portal_toggle(void)
{
    if (s_connecting) return;
    if (s_active) {
        portal_stop();
        return;
    }
    /* A just-closed DNS task must release port 53 before it is restarted. */
    for (int i = 0; (s_dns_alive || s_http_alive) && i < 15; i++)
        vTaskDelay(pdMS_TO_TICKS(100));
    if (s_dns_alive || s_http_alive) {
        printf("GW018 setup: previous portal tasks still stopping\n");
        return;
    }
    rtw_wifi_setting_t current = {0};
    int channel = 1;
    if (wifi_get_setting("wlan0", &current) == 0 &&
        current.channel >= 1 && current.channel <= 14)
        channel = current.channel;
    memset(&current, 0, sizeof(current));
    if (wifi_set_mode(RTW_MODE_STA_AP) != 0) {
        printf("GW018 setup: concurrent Wi-Fi mode failed\n");
        return;
    }
    if (wifi_start_ap(SETUP_SSID, RTW_SECURITY_OPEN, NULL,
                      sizeof(SETUP_SSID) - 1, 0, channel) != RTW_SUCCESS) {
        wifi_set_mode(RTW_MODE_STA);
        printf("GW018 setup: AP start failed\n");
        return;
    }
    char started_ssid[33] = {0};
    int ap_ready = 0;
    for (int i = 0; i < 40; i++) {
        memset(started_ssid, 0, sizeof(started_ssid));
        if (wext_get_ssid(WLAN1_NAME, (unsigned char *)started_ssid) > 0 &&
            strcmp(started_ssid, SETUP_SSID) == 0) {
            ap_ready = 1;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!ap_ready) {
        wifi_set_mode(RTW_MODE_STA);
        printf("GW018 setup: AP did not become ready\n");
        return;
    }
    LwIP_UseStaticIP(&xnetif[1]);
    netif_set_up(&xnetif[1]);
    netif_set_link_up(&xnetif[1]);
    dhcps_init(&xnetif[1]);
    /* The SDK DHCP server also starts a DNS server that only answers one
       example hostname. Replace that DNS service with the captive wildcard. */
    extern void dns_server_deinit(void);
    dns_server_deinit();
    s_active = 1;
    s_last_failed = 0;
    s_started_at = xTaskGetTickCount();
    s_scan_count = 0;
    s_scan_done = 1;
    s_scan_failed = 0;
    s_scan_reported = 1;
    if (!s_dns_alive) {
        s_dns_running = 1;
        s_dns_alive = 1;
        if (xTaskCreate(dns_task, "gw018_dns", 768, NULL,
                        tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
            s_dns_alive = 0;
            s_dns_running = 0;
            portal_stop();
            printf("GW018 setup: DNS task failed\n");
            return;
        }
    }
    s_http_alive = 1;
    if (xTaskCreate(http_task, "gw018_http", 1536, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        s_http_alive = 0;
        portal_stop();
        printf("GW018 setup: HTTP task failed\n");
        return;
    }
    printf("GW018 setup: join %s and open http://%s/\n", SETUP_SSID, SETUP_IP);
    start_scan();
}

void gw018_portal_tick(void)
{
    if (s_active && !s_scan_done &&
        (TickType_t)(xTaskGetTickCount() - s_scan_started_at) >
            pdMS_TO_TICKS(20000)) {
        s_scan_failed = 1;
        s_scan_done = 1;
    }
    if (s_active && s_scan_done && !s_scan_reported) {
        s_scan_reported = 1;
    }
    if (s_active && !s_connecting &&
        (TickType_t)(xTaskGetTickCount() - s_started_at) >
            pdMS_TO_TICKS(SETUP_TIMEOUT_MS)) portal_stop();
}

static int send_all(int fd, const char *data, size_t len)
{
    while (len) {
        int sent = send(fd, data, len, 0);
        if (sent <= 0) return -1;
        data += sent;
        len -= sent;
    }
    return 0;
}

static void reply(int fd, const char *status, const char *body)
{
    char headers[210];
    size_t len = strlen(body);
    int n = snprintf(headers, sizeof(headers),
                     "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n"
                     "Content-Length: %d\r\n\r\n", status, (int)len);
    if (n > 0 && n < sizeof(headers)) {
        int header_sent = send_all(fd, headers, n);
        int body_sent = header_sent == 0 ? send_all(fd, body, len) : -1;
    }
}

static const char *setup_page(void)
{
    int n = snprintf(s_page, sizeof(s_page),
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>GW018 Wi-Fi setup</title>%s<style>"
        "body{font:16px sans-serif;background:#101827;color:#fff;max-width:480px;margin:25px auto;padding:16px}"
        "input,select,button{box-sizing:border-box;width:100%%;padding:12px;margin:7px 0;font-size:16px}"
        "button,.button{display:block;background:#1288a6;color:white;border:0;border-radius:6px;"
        "text-align:center;text-decoration:none;padding:12px;margin:7px 0;font-size:16px;box-sizing:border-box}"
        "</style></head><body><h2>GW018 Wi-Fi setup</h2>"
        "<p>Choose a nearby network or type its name. The setup AP closes after 10 minutes.</p>"
        "%s<form id='wifiForm' action='/save' method='post' enctype='application/x-www-form-urlencoded' onsubmit='return sendWifiForm(this)'>"
        "<select name='ssid_pick'>"
        "<option value=''>Select a scanned network</option>",
        s_scan_done ? "" : "<meta http-equiv='refresh' content='2;url=/'>",
        s_last_failed ? "<p>Connection failed. Check the password and try again.</p>" :
        s_connecting ? "<p>Trying the selected network...</p>" : "");
    if (n < 0 || n >= sizeof(s_page)) return "Setup page error";
    for (int i = 0; i < s_scan_count; i++) {
        int added = snprintf(s_page + n, sizeof(s_page) - n,
                             "<option value='%s'>%s</option>", s_scanned[i], s_scanned[i]);
        if (added < 0 || added >= sizeof(s_page) - n) return "Setup page error";
        n += added;
    }
    int added = snprintf(s_page + n, sizeof(s_page) - n,
        "</select><input name='ssid' maxlength='32' placeholder='Or type Wi-Fi SSID'>"
        "<input name='pass' type='password' maxlength='64' placeholder='Wi-Fi password'>"
        "<button type='submit'>Save and connect</button></form>"
        "<a class='button' href='/scan'>Scan nearby networks</a>"
        "<script>function sendWifiForm(f){var s=f.elements['ssid'].value||f.elements['ssid_pick'].value;"
        "var b='ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(f.elements['pass'].value);"
        "var x=new XMLHttpRequest();x.open('POST','/save',true);"
        "x.setRequestHeader('Content-Type','application/x-www-form-urlencoded');"
        "x.onreadystatechange=function(){if(x.readyState===4){document.open();document.write(x.responseText);document.close();}};"
        "x.send(b);return false;}</script>"
        "<p>%s</p><p>If this page did not open automatically, use http://%s/</p>"
        "</body></html>", !s_scan_done ? "Scanning nearby networks..." :
        s_scan_failed ? "Scan failed. Try again or enter the SSID manually." :
        s_scan_count ? "Select a scanned network above." :
        "No scan results yet. Press Scan or enter the SSID manually.", SETUP_IP);
    if (added < 0 || added >= sizeof(s_page) - n) return "Setup page error";
    return s_page;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int field(const char *body, const char *key, char *out, size_t cap)
{
    size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t n = 0;
            while (*p && *p != '&') {
                unsigned char c = (unsigned char)*p++;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    int a = hex_digit(p[0]), b = hex_digit(p[1]);
                    if (!p[0] || !p[1] || a < 0 || b < 0) return -1;
                    c = (a << 4) | b;
                    p += 2;
                }
                if (c == 0 || n + 1 >= cap) return -1;
                out[n++] = c;
            }
            out[n] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (!p) break;
        p++;
    }
    out[0] = 0;
    return 0;
}

static int multipart_field(const char *body, const char *key,
                          char *out, size_t cap)
{
    const char *part = body;
    while ((part = strstr(part, "Content-Disposition:")) != NULL) {
        const char *line_end = strstr(part, "\r\n");
        if (!line_end) return -1;
        const char *headers_end = strstr(line_end, "\r\n\r\n");
        if (!headers_end) return -1;
        const char *name = strstr(part, "name=");
        int matches = 0;
        if (name && name < line_end) {
            name += 5;
            int quoted = *name == '"';
            if (quoted) name++;
            const char *name_end = name;
            while (name_end < line_end &&
                   (quoted ? *name_end != '"' : *name_end != ';' && *name_end != ' '))
                name_end++;
            matches = (size_t)(name_end - name) == strlen(key) &&
                      strncmp(name, key, strlen(key)) == 0;
        }
        const char *value = headers_end + 4;
        const char *value_end = strstr(value, "\r\n--");
        if (!value_end) return -1;
        if (matches) {
            size_t length = (size_t)(value_end - value);
            if (length >= cap) return -1;
            memcpy(out, value, length);
            out[length] = 0;
            return 1;
        }
        part = value_end + 2;
    }
    out[0] = 0;
    return 0;
}

static int request_is_scan(void)
{
    if (strncmp(s_request, "GET ", 4) != 0) return 0;
    const char *path = s_request + 4;
    if (strncmp(path, "/scan", 5) != 0) return 0;
    return path[5] == '/' || path[5] == '?' || path[5] == ' ';
}

static int parse_content_length(const char *header)
{
    const char *p = header + 15;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return -1;
    unsigned int value = 0;
    while (*p >= '0' && *p <= '9') {
        if (value <= 512) value = value * 10 + (unsigned int)(*p - '0');
        p++;
    }
    return value > 512 ? 513 : (int)value;
}

static void connect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    rtw_wifi_setting_t previous = {0};
    int had_previous = wifi_get_setting("wlan0", &previous) == 0 &&
                       previous.ssid[0] != 0;
    wifi_set_autoreconnect(0);
    s_flash_writer = p_write_reconnect_ptr;
    s_trial_profile_valid = 0;
    p_write_reconnect_ptr = profile_capture;
    wifi_disconnect();
    int joined = wifi_connect(s_pending_ssid,
                    s_pending_password[0] ? RTW_SECURITY_WPA2_AES_PSK : RTW_SECURITY_OPEN,
                    s_pending_password, strlen(s_pending_ssid),
                    strlen(s_pending_password), 0, NULL) == RTW_SUCCESS;
    int got_dhcp = joined && LwIP_DHCP(0, DHCP_START) == DHCP_ADDRESS_ASSIGNED;
    p_write_reconnect_ptr = s_flash_writer;
    int committed = got_dhcp && s_trial_profile_valid && s_flash_writer &&
        s_flash_writer((uint8_t *)&s_trial_profile, sizeof(s_trial_profile)) == 0;
    if (committed) {
        wifi_set_autoreconnect(1);
        portal_stop();
        printf("GW018 setup: new Wi-Fi connected and saved\n");
    } else {
        s_last_failed = 1;
        if (had_previous) {
            wifi_disconnect();
            if (wifi_connect((char *)previous.ssid, previous.security_type,
                             (char *)previous.password, strlen((char *)previous.ssid),
                             strlen((char *)previous.password), previous.key_idx,
                             NULL) == RTW_SUCCESS) LwIP_DHCP(0, DHCP_START);
        }
        wifi_set_autoreconnect(1);
        printf("GW018 setup: new Wi-Fi failed; previous profile retained\n");
    }
    memset(&previous, 0, sizeof(previous));
    memset(s_pending_password, 0, sizeof(s_pending_password));
    memset(&s_trial_profile, 0, sizeof(s_trial_profile));
    s_connecting = 0;
    if (committed) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        ota_platform_reset();
    }
    vTaskDelete(NULL);
}

static void gw018_portal_handle_client(int client_fd)
{
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int total = 0, content_len = -1, header_end = -1;
    while (total < sizeof(s_request) - 1) {
        int count = recv(client_fd, s_request + total,
                         sizeof(s_request) - 1 - total, 0);
        if (count <= 0) break;
        total += count;
        s_request[total] = 0;
        char *end = strstr(s_request, "\r\n\r\n");
        if (end) {
            header_end = (int)(end + 4 - s_request);
            if (strncmp(s_request, "POST ", 5) != 0) break;
            char *cl = strstr(s_request, "Content-Length:");
            if (cl && cl < end) content_len = parse_content_length(cl);
            if (content_len < 0 || content_len > 512 ||
                header_end + content_len >= sizeof(s_request)) break;
            if (content_len > 0 && total >= header_end + content_len) break;
        }
    }
    if (header_end >= 0 && strncmp(s_request, "POST ", 5) == 0 &&
        content_len == 0 && total > header_end)
        content_len = total - header_end;
    if (header_end < 0) return;
    if (request_is_scan()) {
        start_scan();
        reply(client_fd, "200 OK", setup_page());
    } else if (strncmp(s_request, "POST /save ", 11) == 0) {
        if (content_len < 0 || total < header_end + content_len || s_connecting) {
            reply(client_fd, "400 Bad Request", "Invalid or busy request");
            return;
        }
        s_request[header_end + content_len] = 0;
        const char *body = s_request + header_end;
        char ssid[33], selected[33], pass[65];
        const char *multipart_type = strstr(s_request, "multipart/form-data");
        int multipart = multipart_type && multipart_type < body;
        int a = multipart ? multipart_field(body, "ssid", ssid, sizeof(ssid)) :
                            field(body, "ssid", ssid, sizeof(ssid));
        int c = multipart ? multipart_field(body, "ssid_pick", selected, sizeof(selected)) :
                            field(body, "ssid_pick", selected, sizeof(selected));
        int b = multipart ? multipart_field(body, "pass", pass, sizeof(pass)) :
                            field(body, "pass", pass, sizeof(pass));
        if (a >= 0 && !ssid[0] && c == 1 && selected[0])
            strcpy(ssid, selected);
        int invalid = a < 0 ? 1 : c < 0 ? 2 : !ssid[0] ? 3 :
                      b < 0 ? 4 : (pass[0] && strlen(pass) < 8) ? 5 : 0;
        if (invalid) {
            reply(client_fd, "400 Bad Request",
                  invalid == 1 || invalid == 2 ? "Invalid SSID encoding or length" :
                  invalid == 3 ? "Wi-Fi SSID missing. Select or type a network name." :
                  invalid == 4 ? "Invalid Wi-Fi password encoding or length" :
                                 "Wi-Fi password is too short");
            return;
        }
        strcpy(s_pending_ssid, ssid);
        strcpy(s_pending_password, pass);
        memset(pass, 0, sizeof(pass));
        s_connecting = 1;
        if (xTaskCreate(connect_task, "gw018_join", 1536, NULL,
                        tskIDLE_PRIORITY + 2, NULL) != pdPASS) {
            s_connecting = 0;
            reply(client_fd, "500 Internal Server Error", "Cannot start Wi-Fi join");
            return;
        }
        reply(client_fd, "200 OK",
              "<html><body><h2>Trying your Wi-Fi</h2>"
              "<p>The setup network will close after a successful connection. "
              "If it stays up, reconnect to it and check the password.</p></body></html>");
    } else {
        reply(client_fd, "200 OK", setup_page());
    }
}

static void http_task(void *arg)
{
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) goto failed;
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(80);
    address.sin_addr.s_addr = inet_addr(SETUP_IP);
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server_fd, 4) != 0) {
        close(server_fd);
        goto failed;
    }
    printf("GW018 setup: HTTP listening on %s:80\n", SETUP_IP);
    while (s_active) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(server_fd, &readable);
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        if (select(server_fd + 1, &readable, NULL, NULL, &timeout) <= 0)
            continue;
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            gw018_portal_handle_client(client_fd);
            close(client_fd);
        }
    }
    close(server_fd);
    s_http_alive = 0;
    vTaskDelete(NULL);
    return;
failed:
    printf("GW018 setup: HTTP listener failed\n");
    portal_stop();
    s_http_alive = 0;
    vTaskDelete(NULL);
}
