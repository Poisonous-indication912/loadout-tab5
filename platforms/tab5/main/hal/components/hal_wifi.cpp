/*
 * SPDX-FileCopyrightText: 2025 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal/hal_esp32.h"
#include <mooncake_log.h>
#include <vector>
#include <memory>
#include <string.h>
#include <bsp/m5stack_tab5.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_wifi_default.h>
#include <esp_wifi_netif.h>
#include <esp_private/wifi.h>
#include <esp_http_server.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_crt_bundle.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <time.h>
#include <esp_system.h>
#include <hal/hal.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <string>
#include <algorithm>

#define TAG "wifi"

#define WIFI_SSID    "LOADOUT"
#define WIFI_PASS    "loadout1234"   // WPA2 (>=8 chars); configurable en el futuro (SD/web)
#define MAX_STA_CONN 4

static bool s_wifi_ap_netif_started = false;

static void wifi_remote_ap_start_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (s_wifi_ap_netif_started || esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi AP start event");
        return;
    }

    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    uint8_t mac[6];
    esp_err_t ret = esp_wifi_get_if_mac(driver, mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_if_mac failed: %s", esp_err_to_name(ret));
        return;
    }

    if (esp_wifi_is_if_ready_when_started(driver)) {
        ret = esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_register_if_rxcb failed: %s", esp_err_to_name(ret));
            return;
        }
    }

    ret = esp_wifi_internal_reg_netstack_buf_cb(esp_netif_netstack_buf_ref, esp_netif_netstack_buf_free);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "netstack cb register failed: %s", esp_err_to_name(ret));
        return;
    }

    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, event_id, data);
    s_wifi_ap_netif_started = true;
}

static void wifi_remote_ap_stop_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (!s_wifi_ap_netif_started && !esp_netif_is_netif_up(netif)) {
        ESP_LOGW(TAG, "ignore duplicate Wi-Fi AP stop event");
        return;
    }

    esp_netif_action_stop(netif, base, event_id, data);
    s_wifi_ap_netif_started = false;
}

static esp_netif_t* create_wifi_remote_ap_netif()
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_t* netif     = esp_netif_new(&cfg);
    assert(netif);

    ESP_ERROR_CHECK(esp_netif_attach_wifi_ap(netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, wifi_remote_ap_start_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP, wifi_remote_ap_stop_handler, netif));

    return netif;
}

/* -------------------------- STA (cliente, internet) ----------------------- */
// Estado de la conexion STA (a tu router de casa). Lo leen el HAL/Settings.
static bool s_sta_started   = false;
static bool s_sta_connected = false;
static char s_sta_ssid[33]  = {0};
static char s_sta_ip[16]    = {0};
static bool s_ntp_started   = false;

// NTP sincronizado -> aplica zona horaria (config "tz", POSIX) y pone el RTC en hora.
static void ntp_sync_cb(struct timeval* tv)
{
    std::string tz = GetHAL()->getSettingStr("tz", "UTC0");
    setenv("TZ", tz.c_str(), 1);
    tzset();
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    GetHAL()->setRtcTime(lt);
    ESP_LOGI(TAG, "NTP -> RTC: %04d-%02d-%02d %02d:%02d (tz=%s)",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, tz.c_str());
}

static void wifi_remote_sta_start_handler(void* arg, esp_event_base_t base, int32_t event_id, void* data)
{
    auto* netif = static_cast<esp_netif_t*>(arg);
    if (s_sta_started) return;
    auto driver = static_cast<wifi_netif_driver_t>(esp_netif_get_io_driver(netif));
    uint8_t mac[6];
    if (esp_wifi_get_if_mac(driver, mac) != ESP_OK) return;
    if (esp_wifi_is_if_ready_when_started(driver)) {
        if (esp_wifi_register_if_rxcb(driver, esp_netif_receive, netif) != ESP_OK) return;
    }
    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, event_id, data);
    s_sta_started = true;
    esp_wifi_connect();
}

static void wifi_remote_sta_event_handler(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        s_sta_ip[0]     = 0;
        esp_wifi_connect();  // reintento
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_connected = true;
        ESP_LOGI(TAG, "STA conectado, IP: %s", s_sta_ip);
        // NTP: poner la hora automaticamente (una vez). El cb pone el RTC.
        if (!s_ntp_started) {
            s_ntp_started = true;
            esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            c.sync_cb = ntp_sync_cb;
            esp_netif_sntp_init(&c);
        }
    }
}

static esp_netif_t* create_wifi_remote_sta_netif()
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t* netif     = esp_netif_new(&cfg);
    assert(netif);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_remote_sta_start_handler, netif));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_remote_sta_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_remote_sta_event_handler, nullptr));
    return netif;
}

// HTTP 处理函数
// Dashboard LOADOUT (gestion del Tab5 desde el navegador)
esp_err_t dash_get_handler(httpd_req_t* req)
{
    const char* html = R"rawliteral(<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><title>LOADOUT</title>
<style>
body{background:#070b0a;color:#cfeede;font-family:ui-monospace,monospace;margin:0;padding:22px}
h1{color:#bfe9cf;letter-spacing:2px}
.card{background:#0f1a16;border:1px solid #2f6f4f;border-radius:12px;padding:16px 18px;margin:14px 0}
.lab{color:#9be8b8;font-size:13px;text-transform:uppercase;letter-spacing:1px}
.v{color:#38ef7d;font-size:30px}
input[type=range]{width:100%;accent-color:#38ef7d;margin-top:10px}
input[type=text],input[type=file]{width:100%;box-sizing:border-box;background:#0a120f;color:#cfeede;border:1px solid #2f6f4f;border-radius:8px;padding:10px;margin-top:8px}
button{background:#16382b;color:#38ef7d;border:1px solid #2f6f4f;border-radius:8px;padding:10px 18px;font-size:15px;letter-spacing:1px;margin-top:10px;cursor:pointer}
button.danger{color:#ff5f57}
ul{list-style:none;padding:0;margin:8px 0 0}
li{padding:6px 0;color:#43d2ff;border-bottom:1px solid #15241d}
#st{color:#fdbe1a;font-size:13px;min-height:16px}
</style></head><body>
<h1>// LOADOUT</h1>
<div class=card><div class=lab>Power</div><span class=v id=batt>--</span>% &nbsp;&nbsp;<span id=va class=lab style="font-size:16px">--</span></div>
<div class=card><div class=lab>LCD Brightness <span class=v id=brv style="font-size:20px">--</span></div>
<input type=range min=0 max=100 id=br oninput="set('bright',this.value)"></div>
<div class=card><div class=lab>Speaker Volume <span class=v id=vov style="font-size:20px">--</span></div>
<input type=range min=0 max=100 id=vo oninput="set('vol',this.value)"></div>
<div class=card><div class=lab>Firmwares on SD</div><ul id=fwl></ul>
<input type=file id=fwf accept=".bin"><button onclick="up()">UPLOAD TO SD</button>
<div id=st></div></div>
<div class=card><div class=lab>AP password (LOADOUT, min 8 chars)</div>
<input type=text id=pw placeholder="new AP password">
<button onclick="savepw()">SAVE &amp; REBOOT</button></div>
<div class=card><div class=lab>Home WiFi (internet, for OTA)</div>
<input type=text id=ss placeholder="home SSID">
<input type=text id=sp placeholder="home password">
<button onclick="savesta()">CONNECT &amp; REBOOT</button></div>
<div class=card><button class=danger onclick="if(confirm('Reboot Tab5?'))fetch('/api/reboot')">REBOOT</button></div>
<script>
function set(k,v){fetch('/api/set?'+k+'='+v)}
async function fws(){try{let d=await(await fetch('/api/fw/list')).json();
fwl.innerHTML=d.length?d.map(f=>'<li>'+f+'</li>').join(''):'<li style=color:#7a8a82>none</li>';}catch(e){}}
function up(){let f=fwf.files[0];if(!f){st.textContent='Pick a .bin first';return;}
st.textContent='Uploading '+f.name+' ...';
fetch('/api/fw/upload?name='+encodeURIComponent(f.name),{method:'POST',body:f})
.then(r=>r.text()).then(t=>{st.textContent=t;fws();}).catch(e=>{st.textContent='Upload failed';});}
function savepw(){let p=pw.value;if(p.length<8){st.textContent='Password too short';return;}
fetch('/api/wifi?pass='+encodeURIComponent(p)).then(r=>r.text()).then(t=>{st.textContent=t;});}
function savesta(){if(!ss.value){st.textContent='Enter home SSID';return;}
fetch('/api/wifi?sta_ssid='+encodeURIComponent(ss.value)+'&sta_pass='+encodeURIComponent(sp.value))
.then(r=>r.text()).then(t=>{st.textContent=t;});}
async function poll(){try{let d=await(await fetch('/api/status')).json();
batt.textContent=d.batt;va.textContent=d.v.toFixed(2)+'V  '+d.a.toFixed(2)+'A';
brv.textContent=d.bright;vov.textContent=d.vol;
if(document.activeElement!=br)br.value=d.bright;
if(document.activeElement!=vo)vo.value=d.vol;}catch(e){}}
setInterval(poll,1500);poll();fws();
</script></body></html>)rawliteral";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t status_get_handler(httpd_req_t* req)
{
    float v   = GetHAL()->powerMonitorData.busVoltage;
    float a   = GetHAL()->powerMonitorData.shuntCurrent;
    int   br  = GetHAL()->getDisplayBrightness();
    int   vol = GetHAL()->getSpeakerVolume();
    int   pct = (int)((v - 6.0f) / 2.4f * 100.0f);
    if (pct < 0) { pct = 0; }
    if (pct > 100) { pct = 100; }
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"v\":%.2f,\"a\":%.2f,\"batt\":%d,\"bright\":%d,\"vol\":%d}", v, a, pct, br, vol);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t set_get_handler(httpd_req_t* req)
{
    char q[64], val[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "bright", val, sizeof(val)) == ESP_OK) {
            GetHAL()->setDisplayBrightness((uint8_t)atoi(val));
        }
        if (httpd_query_key_value(q, "vol", val, sizeof(val)) == ESP_OK) {
            GetHAL()->setSpeakerVolume((uint8_t)atoi(val));
        }
    }
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

esp_err_t reboot_get_handler(httpd_req_t* req)
{
    httpd_resp_sendstr(req, "rebooting");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// Lista los .bin de la microSD (JSON array).
esp_err_t fw_list_handler(httpd_req_t* req)
{
    std::string json = "[";
    if (bsp_sdcard_init((char*)"/sd", 5) == ESP_OK) {
        DIR* d = opendir("/sd");
        if (d) {
            struct dirent* e;
            bool first = true;
            while ((e = readdir(d)) != nullptr) {
                std::string n = e->d_name;
                if (n.size() > 4 && n.substr(n.size() - 4) == ".bin") {
                    if (!first) json += ",";
                    json += "\"" + n + "\"";
                    first = false;
                }
            }
            closedir(d);
        }
        bsp_sdcard_deinit((char*)"/sd");
    }
    json += "]";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Sube un fichero a la SD: POST /api/fw/upload?name=foo.bin con el binario en el body.
esp_err_t fw_upload_handler(httpd_req_t* req)
{
    char q[160], name[96] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK || name[0] == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
        return ESP_FAIL;
    }
    // Saneado basico: sin separadores de ruta.
    for (char* p = name; *p; p++) if (*p == '/' || *p == '\\') *p = '_';

    if (bsp_sdcard_init((char*)"/sd", 5) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no SD");
        return ESP_FAIL;
    }
    std::string path = std::string("/sd/") + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        bsp_sdcard_deinit((char*)"/sd");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }
    char buf[2048];
    int remaining = req->content_len;
    bool ok = true;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, std::min((int)sizeof(buf), remaining));
        if (r <= 0) { ok = false; break; }
        if ((int)fwrite(buf, 1, r, f) != r) { ok = false; break; }
        remaining -= r;
    }
    fclose(f);
    bsp_sdcard_deinit((char*)"/sd");
    if (!ok) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed"); return ESP_FAIL; }
    std::string msg = std::string("Saved ") + name;
    httpd_resp_sendstr(req, msg.c_str());
    return ESP_OK;
}

// Config WiFi (NVS) + reinicio. Acepta: pass (clave del AP), sta_ssid, sta_pass
// (tu WiFi de casa para internet). Cualquiera de ellos.
esp_err_t wifi_set_handler(httpd_req_t* req)
{
    char q[256], val[96];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_sendstr(req, "missing params"); return ESP_OK;
    }
    bool changed = false;
    if (httpd_query_key_value(q, "pass", val, sizeof(val)) == ESP_OK && val[0]) {
        if (strlen(val) < 8) { httpd_resp_sendstr(req, "AP password too short"); return ESP_OK; }
        GetHAL()->setSettingStr("wifi_pass", val); changed = true;
    }
    if (httpd_query_key_value(q, "sta_ssid", val, sizeof(val)) == ESP_OK && val[0]) {
        GetHAL()->setSettingStr("sta_ssid", val); changed = true;
    }
    if (httpd_query_key_value(q, "sta_pass", val, sizeof(val)) == ESP_OK) {
        GetHAL()->setSettingStr("sta_pass", val); changed = true;
    }
    if (changed) {
        httpd_resp_sendstr(req, "Saved. Rebooting to apply ...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        httpd_resp_sendstr(req, "nothing to change");
    }
    return ESP_OK;
}

static httpd_uri_t dash_uri   = {.uri = "/",               .method = HTTP_GET,  .handler = dash_get_handler,   .user_ctx = nullptr};
static httpd_uri_t status_uri = {.uri = "/api/status",     .method = HTTP_GET,  .handler = status_get_handler, .user_ctx = nullptr};
static httpd_uri_t set_uri    = {.uri = "/api/set",        .method = HTTP_GET,  .handler = set_get_handler,    .user_ctx = nullptr};
static httpd_uri_t reboot_uri = {.uri = "/api/reboot",     .method = HTTP_GET,  .handler = reboot_get_handler, .user_ctx = nullptr};
static httpd_uri_t fwlist_uri = {.uri = "/api/fw/list",    .method = HTTP_GET,  .handler = fw_list_handler,    .user_ctx = nullptr};
static httpd_uri_t fwup_uri   = {.uri = "/api/fw/upload",  .method = HTTP_POST, .handler = fw_upload_handler,  .user_ctx = nullptr};
static httpd_uri_t wifi_uri   = {.uri = "/api/wifi",       .method = HTTP_GET,  .handler = wifi_set_handler,   .user_ctx = nullptr};

// 启动 Web Server
httpd_handle_t start_webserver()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size       = 8192;  // upload + SD necesitan algo mas de pila
    httpd_handle_t server = nullptr;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &dash_uri);
        httpd_register_uri_handler(server, &status_uri);
        httpd_register_uri_handler(server, &set_uri);
        httpd_register_uri_handler(server, &reboot_uri);
        httpd_register_uri_handler(server, &fwlist_uri);
        httpd_register_uri_handler(server, &fwup_uri);
        httpd_register_uri_handler(server, &wifi_uri);
    }
    return server;
}

// 初始化 Wi-Fi AP 模式
// Lee una clave "key=valor" de una cadena de config (una por linea). "" si no esta.
static std::string conf_get(const std::string& conf, const std::string& key)
{
    // Busqueda POR LINEAS (no por subcadena): la clave debe estar al principio
    // de su linea, ignorando lineas de comentario (#). Asi un "pin=clear" dentro
    // de un comentario NO se confunde con la clave real.
    size_t pos = 0;
    while (pos < conf.size()) {
        size_t eol = conf.find_first_of("\r\n", pos);
        std::string line = conf.substr(pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
        pos = (eol == std::string::npos) ? conf.size() : eol + 1;
        // trim inicial
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        if (line[b] == '#') continue;                       // comentario
        std::string pat = key + "=";
        if (line.compare(b, pat.size(), pat) != 0) continue; // no es esta clave
        std::string v = line.substr(b + pat.size());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        return v;
    }
    return "";
}

void wifi_init_softap()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    create_wifi_remote_ap_netif();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Config desde /sd/loadout.conf (prioridad) -> NVS. Persistimos en NVS lo que
    // venga del fichero para los siguientes arranques.
    std::string conf = GetHAL()->readTextFile("/sd/loadout.conf");

    // --- Password del AP LOADOUT ---
    std::string pass = GetHAL()->getSettingStr("wifi_pass", WIFI_PASS);
    {
        std::string p = conf_get(conf, "wifi_pass");
        if (p.size() >= 8) { pass = p; GetHAL()->setSettingStr("wifi_pass", p); }
    }
    if (pass.size() < 8) pass = WIFI_PASS;

    // --- Credenciales STA (tu WiFi de casa) ---
    std::string staSsid = GetHAL()->getSettingStr("sta_ssid", "");
    std::string staPass = GetHAL()->getSettingStr("sta_pass", "");
    {
        std::string s = conf_get(conf, "wifi_sta_ssid");
        std::string p = conf_get(conf, "wifi_sta_pass");
        if (!s.empty()) { staSsid = s; GetHAL()->setSettingStr("sta_ssid", s); }
        if (!p.empty()) { staPass = p; GetHAL()->setSettingStr("sta_pass", p); }
    }
    bool useSta = !staSsid.empty();

    // --- Tema (persistente; tambien editable desde el config) ---
    int theme = GetHAL()->getSettingInt("theme", 0);
    {
        std::string tv = conf_get(conf, "theme");
        if (!tv.empty()) { theme = atoi(tv.c_str()); GetHAL()->setSettingInt("theme", theme); }
    }

    // --- URL del manifest OTA (latest.json en tu repo GitHub) ---
    std::string otaUrl = GetHAL()->getSettingStr("ota_url", "");
    {
        std::string u = conf_get(conf, "ota_url");
        if (!u.empty()) { otaUrl = u; GetHAL()->setSettingStr("ota_url", u); }
    }

    // --- Zona horaria (POSIX TZ) para el NTP ---
    {
        std::string tz = conf_get(conf, "tz");
        if (!tz.empty()) GetHAL()->setSettingStr("tz", tz);
    }

    // --- Chatbot IA (endpoint OpenAI-compatible + key + modelo) ---
    {
        std::string u = conf_get(conf, "ai_url");
        std::string k = conf_get(conf, "ai_key");
        std::string m = conf_get(conf, "ai_model");
        if (!u.empty()) GetHAL()->setSettingStr("ai_url", u);
        if (!k.empty()) GetHAL()->setSettingStr("ai_key", k);
        if (!m.empty()) GetHAL()->setSettingStr("ai_model", m);
    }

    // --- PIN de bloqueo (4-8 digitos). Recuperacion: pin=clear lo desactiva.
    //     Solo se toca NVS si la linea trae un valor util (asi un PIN puesto
    //     desde Settings NO se borra por no estar en el .conf). ---
    {
        std::string pv = conf_get(conf, "pin");
        if (pv == "clear" || pv == "reset" || pv == "none") {
            GetHAL()->setSettingStr("pin", "");
        } else {
            bool digits = pv.size() >= 4 && pv.size() <= 8;
            for (char c : pv) if (c < '0' || c > '9') digits = false;
            if (digits) GetHAL()->setSettingStr("pin", pv);
        }
    }

    // --- Crear /sd/loadout.conf por defecto si no existe (con los valores actuales) ---
    if (conf.empty()) {
        char tmpl[1280];
        snprintf(tmpl, sizeof(tmpl),
                 "# LOADOUT config - edita y reinicia\r\n"
                 "# Password del AP LOADOUT (min 8 chars):\r\n"
                 "wifi_pass=%s\r\n\r\n"
                 "# Tu WiFi de casa (internet / OTA):\r\n"
                 "wifi_sta_ssid=%s\r\n"
                 "wifi_sta_pass=%s\r\n\r\n"
                 "# Tema: 0=Default 1=Solarized Dark 2=Solarized Light 3=Amber Mono\r\n"
                 "theme=%d\r\n\r\n"
                 "# Zona horaria (POSIX) para NTP. Espana: CET-1CEST,M3.5.0,M10.5.0/3\r\n"
                 "tz=UTC0\r\n\r\n"
                 "# OTA: URL del manifest latest.json de tu repo GitHub\r\n"
                 "# ota_url=https://github.com/USER/REPO/releases/latest/download/latest.json\r\n"
                 "ota_url=%s\r\n\r\n"
                 "# Chatbot IA (endpoint OpenAI-compatible: DeepSeek/Qwen/OpenAI/Groq/Ollama)\r\n"
                 "# ai_url=https://api.deepseek.com/chat/completions\r\n"
                 "# ai_key=sk-...\r\n"
                 "# ai_model=deepseek-chat\r\n"
                 "ai_url=\r\nai_key=\r\nai_model=deepseek-chat\r\n\r\n"
                 "# PIN de bloqueo (4-8 digitos). Vacio o 'pin=clear' = sin bloqueo.\r\n"
                 "# (recuperacion: si te bloqueas, pon pin=clear y reinicia)\r\n"
                 "pin=\r\n",
                 pass.c_str(), staSsid.c_str(), staPass.c_str(), theme, otaUrl.c_str());
        GetHAL()->writeTextFile("/sd/loadout.conf", tmpl);
        ESP_LOGI(TAG, "Creado /sd/loadout.conf por defecto");
    }

    if (useSta) create_wifi_remote_sta_netif();

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.ssid), WIFI_SSID, sizeof(ap_config.ap.ssid));
    std::strncpy(reinterpret_cast<char*>(ap_config.ap.password), pass.c_str(), sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len       = std::strlen(WIFI_SSID);
    ap_config.ap.max_connection = MAX_STA_CONN;
    ap_config.ap.authmode       = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(useSta ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    if (useSta) {
        wifi_config_t sta_config = {};
        std::strncpy(reinterpret_cast<char*>(sta_config.sta.ssid), staSsid.c_str(), sizeof(sta_config.sta.ssid) - 1);
        std::strncpy(reinterpret_cast<char*>(sta_config.sta.password), staPass.c_str(), sizeof(sta_config.sta.password) - 1);
        std::strncpy(s_sta_ssid, staSsid.c_str(), sizeof(s_sta_ssid) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    // Bajar la potencia de TX (def ~20dBm -> ~10dBm) para reducir el pico de
    // corriente del radio: ayuda con los brownouts si la alimentacion es flojita.
    esp_wifi_set_max_tx_power(40);  // unidades de 0.25 dBm -> 40 = 10 dBm
    ESP_LOGI(TAG, "Wi-Fi AP:%s  STA:%s (tx 10dBm)", WIFI_SSID, useSta ? staSsid.c_str() : "(off)");
}

static void wifi_ap_test_task(void* param)
{
    wifi_init_softap();
    start_webserver();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

/* ------------------------------- OTA update ------------------------------- */
// Estados: 0 idle, 1 checking, 2 checked(hay info), 3 updating, 4 error
static volatile int s_ota_state = 0;
static char        s_ota_latest[24] = {0};
static char        s_ota_msg[120]   = {0};
static std::string s_ota_dl_url;

// Acumula el body en un std::string (sigue redirects con esp_http_client_perform).
static esp_err_t http_evt(esp_http_client_event_t* e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data && e->data_len > 0) {
        auto* out = static_cast<std::string*>(e->user_data);
        out->append((const char*)e->data, e->data_len);
    }
    return ESP_OK;
}

static std::string http_get(const std::string& url)
{
    std::string body;
    esp_http_client_config_t cfg = {};
    cfg.url               = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 9000;
    cfg.event_handler     = http_evt;
    cfg.user_data         = &body;
    cfg.disable_auto_redirect = false;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return "";
    esp_err_t err = esp_http_client_perform(c);
    int status    = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || status < 200 || status >= 400) return "";
    return body;
}

// Extrae el valor de "key":"valor" de un JSON simple.
static std::string json_str(const std::string& j, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k = j.find(pat);
    if (k == std::string::npos) return "";
    size_t c = j.find(':', k + pat.size());
    if (c == std::string::npos) return "";
    size_t q1 = j.find('"', c);
    if (q1 == std::string::npos) return "";
    size_t q2 = j.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return j.substr(q1 + 1, q2 - q1 - 1);
}

static void ota_check_task(void*)
{
    s_ota_state = 1;
    std::string url = GetHAL()->getSettingStr("ota_url", "");
    if (url.empty()) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Set ota_url in /sd/loadout.conf");
        s_ota_state = 4; vTaskDelete(NULL); return;
    }
    if (!s_sta_connected) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Connect home WiFi first");
        s_ota_state = 4; vTaskDelete(NULL); return;
    }
    std::string j = http_get(url);
    if (j.empty()) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Manifest fetch failed");
        s_ota_state = 4; vTaskDelete(NULL); return;
    }
    std::string ver = json_str(j, "version");
    std::string durl = json_str(j, "url");
    std::string notes = json_str(j, "notes");
    if (ver.empty() || durl.empty()) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Bad manifest");
        s_ota_state = 4; vTaskDelete(NULL); return;
    }
    strncpy(s_ota_latest, ver.c_str(), sizeof(s_ota_latest) - 1);
    s_ota_dl_url = durl;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "%s", notes.empty() ? "ready" : notes.c_str());
    s_ota_state = 2;
    vTaskDelete(NULL);
}

static void ota_update_task(void*)
{
    s_ota_state = 3;
    snprintf(s_ota_msg, sizeof(s_ota_msg), "Connecting...");
    esp_http_client_config_t http = {};
    http.url               = s_ota_dl_url.c_str();
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms        = 20000;
    http.keep_alive_enable = true;
    esp_https_ota_config_t cfg = {};
    cfg.http_config = &http;

    esp_https_ota_handle_t h = nullptr;
    esp_err_t e = esp_https_ota_begin(&cfg, &h);
    if (e != ESP_OK) {
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Connect failed: %s", esp_err_to_name(e));
        s_ota_state = 4; vTaskDelete(NULL); return;
    }
    int total = esp_https_ota_get_image_size(h);
    while (true) {
        e = esp_https_ota_perform(h);
        if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        int read = esp_https_ota_get_image_len_read(h);
        int pct  = (total > 0) ? (read * 100 / total) : 0;
        snprintf(s_ota_msg, sizeof(s_ota_msg), "Downloading & flashing... %d%%", pct);
    }
    if (e == ESP_OK && esp_https_ota_is_complete_data_received(h)) {
        e = esp_https_ota_finish(h);  // valida + set_boot_partition
        if (e == ESP_OK) {
            snprintf(s_ota_msg, sizeof(s_ota_msg), "Done. Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(700));
            esp_restart();
        }
    } else {
        esp_https_ota_abort(h);
    }
    snprintf(s_ota_msg, sizeof(s_ota_msg), "OTA failed: %s", esp_err_to_name(e));
    s_ota_state = 4;
    vTaskDelete(NULL);
}

void HalEsp32::otaStartCheck()
{
    if (s_ota_state == 1 || s_ota_state == 3) return;
    s_ota_latest[0] = 0; s_ota_msg[0] = 0; s_ota_dl_url.clear();
    xTaskCreate(ota_check_task, "otachk", 8192, nullptr, 5, nullptr);
}

void HalEsp32::otaStartUpdate()
{
    if (s_ota_state == 3 || s_ota_dl_url.empty()) return;
    xTaskCreate(ota_update_task, "otaup", 8192, nullptr, 5, nullptr);
}

int HalEsp32::otaState() { return s_ota_state; }
std::string HalEsp32::otaLatestVersion() { return s_ota_latest; }
std::string HalEsp32::otaMessage() { return s_ota_msg; }

/* ------------------------------- Chatbot IA ------------------------------- */
// API estilo OpenAI (DeepSeek/Qwen/OpenAI/Groq/Ollama...): POST {model,messages}
// con Authorization: Bearer. Config en /sd/loadout.conf: ai_url, ai_key, ai_model.
static volatile int s_chat_state = 0;  // 0 idle, 1 sending, 2 done, 3 error
static std::string  s_chat_reply;
#include <vector>
#include <utility>
static std::vector<std::pair<std::string, std::string>> s_chat_hist;  // (role, content)
static const size_t CHAT_HIST_MAX = 12;  // ultimos N mensajes (memoria de conversacion)

static std::string json_escape(const std::string& s)
{
    std::string o; o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                else o += c;
        }
    }
    return o;
}

static std::string json_unescape(const std::string& s)
{
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            switch (n) {
                case 'n': o += '\n'; break;
                case 'r': o += '\r'; break;
                case 't': o += '\t'; break;
                case '"': o += '"';  break;
                case '\\': o += '\\'; break;
                case '/': o += '/';  break;
                case 'u': if (i + 4 < s.size()) { i += 4; o += '?'; } break;  // unicode -> ?
                default: o += n;
            }
        } else o += s[i];
    }
    return o;
}

// Devuelve el valor (desescapado) de la PRIMERA "key":"valor" respetando comillas escapadas.
static std::string json_get_string(const std::string& j, const char* key)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k = j.find(pat);
    if (k == std::string::npos) return "";
    size_t c = j.find(':', k + pat.size());
    if (c == std::string::npos) return "";
    size_t q = j.find('"', c);
    if (q == std::string::npos) return "";
    std::string esc;
    for (size_t i = q + 1; i < j.size(); i++) {
        if (j[i] == '\\' && i + 1 < j.size()) { esc += j[i]; esc += j[i + 1]; i++; continue; }
        if (j[i] == '"') break;
        esc += j[i];
    }
    return json_unescape(esc);
}

static void chat_task(void*)
{
    s_chat_state = 1;
    std::string url   = GetHAL()->getSettingStr("ai_url", "");
    std::string key   = GetHAL()->getSettingStr("ai_key", "");
    std::string model = GetHAL()->getSettingStr("ai_model", "deepseek-chat");
    if (url.empty() || key.empty()) {
        s_chat_reply = "Set ai_url / ai_key in /sd/loadout.conf"; s_chat_state = 3; vTaskDelete(NULL); return;
    }
    if (!s_sta_connected) {
        s_chat_reply = "Connect home WiFi first (Settings)"; s_chat_state = 3; vTaskDelete(NULL); return;
    }
    // Construir messages[] con todo el historial (memoria de conversacion).
    std::string msgs;
    for (size_t i = 0; i < s_chat_hist.size(); i++) {
        if (i) msgs += ",";
        msgs += "{\"role\":\"" + s_chat_hist[i].first + "\",\"content\":\"" +
                json_escape(s_chat_hist[i].second) + "\"}";
    }
    std::string body = "{\"model\":\"" + model + "\",\"messages\":[" + msgs + "],\"stream\":false}";
    std::string resp;
    esp_http_client_config_t cfg = {};
    cfg.url               = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 30000;
    cfg.event_handler     = http_evt;
    cfg.user_data         = &resp;
    cfg.method            = HTTP_METHOD_POST;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    std::string auth = "Bearer " + key;
    esp_http_client_set_header(c, "Authorization", auth.c_str());
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, body.c_str(), body.size());
    esp_err_t e = esp_http_client_perform(c);
    int status  = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (e != ESP_OK) {
        s_chat_reply = std::string("Network error: ") + esp_err_to_name(e); s_chat_state = 3;
        vTaskDelete(NULL); return;
    }
    std::string content = json_get_string(resp, "content");
    if (!content.empty()) {
        s_chat_reply = content;
        s_chat_hist.push_back({"assistant", content});  // recordar la respuesta
        while (s_chat_hist.size() > CHAT_HIST_MAX) s_chat_hist.erase(s_chat_hist.begin());
        s_chat_state = 2;
    } else {
        std::string err = json_get_string(resp, "message");
        s_chat_reply = err.empty() ? (std::string("HTTP ") + std::to_string(status)) : err;
        s_chat_state = 3;
    }
    vTaskDelete(NULL);
}

void HalEsp32::chatSend(const std::string& prompt)
{
    if (s_chat_state == 1) return;  // ocupado
    s_chat_reply.clear();
    s_chat_hist.push_back({"user", prompt});  // añadir al historial
    while (s_chat_hist.size() > CHAT_HIST_MAX) s_chat_hist.erase(s_chat_hist.begin());
    xTaskCreate(chat_task, "chat", 10240, nullptr, 5, nullptr);
}
int HalEsp32::chatState() { return s_chat_state; }
std::string HalEsp32::chatReply() { return s_chat_reply; }
void HalEsp32::chatReset() { if (s_chat_state != 1) s_chat_state = 0; }

bool HalEsp32::wifiStaConnected()
{
    return s_sta_connected;
}

std::string HalEsp32::wifiStaInfo()
{
    if (s_sta_ssid[0] == 0) return "not configured (edit /sd/loadout.conf)";
    if (s_sta_connected) return std::string(s_sta_ssid) + "  " + s_sta_ip;
    return std::string(s_sta_ssid) + "  (connecting...)";
}

bool HalEsp32::wifi_init()
{
    mclog::tagInfo(TAG, "wifi init");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 12KB: wifi_init_softap monta la SD (FATFS), lee/escribe loadout.conf y hace
    // setup AP+STA; con 4096 desbordaba la pila (stack overflow -> reboot en bucle).
    xTaskCreate(wifi_ap_test_task, "ap", 12288, nullptr, 5, nullptr);
    return true;
}

void HalEsp32::setExtAntennaEnable(bool enable)
{
    _ext_antenna_enable = enable;
    mclog::tagInfo(TAG, "set ext antenna enable: {}", _ext_antenna_enable);
    bsp_set_ext_antenna_enable(_ext_antenna_enable);
}

bool HalEsp32::getExtAntennaEnable()
{
    return _ext_antenna_enable;
}

void HalEsp32::startWifiAp()
{
    wifi_init();
}
