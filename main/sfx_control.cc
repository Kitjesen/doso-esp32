#include "sfx_control.h"
#include "usb_protocol.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "sdkconfig.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <string>
#include <string_view>
#include <mutex>
#include <vector>

#if __has_include("hal/usb_serial_jtag_ll.h")
#include "hal/usb_serial_jtag_ll.h"
#define SFX_HAS_USJ_LL 1
#else
#define SFX_HAS_USJ_LL 0
#endif

#define TAG "SfxControl"
#define SFX_HTTP_PORT 8080

namespace {

struct SfxEntry {
    const char* id;
    const char* label;
    std::string_view (*sound)();
};

// Accessors keep the string_view objects from being copied as temps in the table.
#define SFX_ITEM(id, label, symbol) \
    {id, label, []() -> std::string_view { return Lang::Sounds::symbol; }}

const SfxEntry kSfx[] = {
    SFX_ITEM("boot_sequence", "开机整段", OGG_SFX_BOOT_SEQUENCE),
    SFX_ITEM("boot_start", "开机开始", OGG_SFX_BOOT_START),
    SFX_ITEM("boot_ready", "开机就绪", OGG_SFX_BOOT_READY),
    SFX_ITEM("precheck_item", "预检项目", OGG_SFX_PRECHECK_ITEM),
    SFX_ITEM("precheck_progress", "预检进行", OGG_SFX_PRECHECK_PROGRESS),
    SFX_ITEM("precheck_pass", "预检通过", OGG_SFX_PRECHECK_PASS),
    SFX_ITEM("precheck_fail", "预检失败", OGG_SFX_PRECHECK_FAIL),
    SFX_ITEM("wake", "唤醒", OGG_SFX_WAKE),
    SFX_ITEM("listen", "倾听", OGG_SFX_LISTEN),
    SFX_ITEM("command_ack", "指令确认", OGG_SFX_COMMAND_ACK),
    SFX_ITEM("command_reject", "指令拒绝", OGG_SFX_COMMAND_REJECT),
    SFX_ITEM("battery_low", "电量低", OGG_SFX_BATTERY_LOW),
    SFX_ITEM("battery_critical", "电量告急", OGG_SFX_BATTERY_CRITICAL),
    SFX_ITEM("charging", "充电", OGG_SFX_CHARGING),
    SFX_ITEM("connected", "已连接", OGG_SFX_CONNECTED),
    SFX_ITEM("disconnected", "已断开", OGG_SFX_DISCONNECTED),
    SFX_ITEM("mode_change", "模式切换", OGG_SFX_MODE_CHANGE),
    SFX_ITEM("obstacle", "障碍", OGG_SFX_OBSTACLE),
    SFX_ITEM("warning", "警告", OGG_SFX_WARNING),
    SFX_ITEM("fall", "摔倒", OGG_SFX_FALL),
    SFX_ITEM("stand_up", "站起", OGG_SFX_STAND_UP),
    SFX_ITEM("estop", "急停", OGG_SFX_ESTOP),
    SFX_ITEM("sleep", "休眠", OGG_SFX_SLEEP),
};

#undef SFX_ITEM

const SfxEntry* FindSfx(const char* id) {
    if (id == nullptr) {
        return nullptr;
    }
    for (const auto& entry : kSfx) {
        if (strcmp(entry.id, id) == 0) {
            return &entry;
        }
    }
    return nullptr;
}

std::string GetStaIp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return "";
    }
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0) {
        return "";
    }
    char buf[16];
    esp_ip4addr_ntoa(&info.ip, buf, sizeof(buf));
    return buf;
}

void AddCors(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
}

esp_err_t SendJson(httpd_req_t* req, const char* json) {
    AddCors(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

// Compact click-to-play page hosted on the board (works when Wi-Fi is up).
const char kIndexHtml[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>机器狗音效</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; font-family:"Segoe UI",sans-serif; background:#111814; color:#e8f0e9; }
  header { padding:20px 24px 8px; }
  h1 { margin:0; font-size:22px; font-weight:650; }
  p { margin:8px 0 0; color:#9bb39e; font-size:14px; }
  #status { margin:12px 24px 0; font-size:13px; color:#8fd18f; }
  .grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(140px,1fr)); gap:10px; padding:20px 24px 40px; }
  button { appearance:none; border:0; border-radius:12px; padding:14px 10px; background:#1d2a20; color:#e8f0e9;
    font-size:15px; cursor:pointer; transition:background .15s, transform .05s; }
  button:hover { background:#2a3d2e; }
  button:active { transform:scale(.97); }
  button.busy { background:#3d5a28; }
  .chat { display:flex; gap:8px; padding:16px 24px 0; }
  .chat input { flex:1; border:0; border-radius:12px; padding:12px 14px; background:#1d2a20; color:#e8f0e9; font-size:15px; }
  .chat button { min-width:72px; }
  .transcript { margin:16px 24px 0; padding:16px 18px; background:#0c120e; border-radius:12px; }
  .transcript .label { font-size:12px; color:#9bb39e; margin-bottom:6px; }
  .stt-text { font-size:20px; font-weight:600; line-height:1.45; min-height:1.45em; }
  .turns { margin-top:12px; display:flex; flex-direction:column; gap:8px; max-height:220px; overflow:auto; }
  .turn { font-size:14px; line-height:1.4; }
  .turn span { display:inline-block; min-width:2.5em; margin-right:8px; }
  .turn.stt span { color:#8fd18f; }
  .turn.tts span { color:#9bb8d1; }
</style>
</head>
<body>
<header>
  <h1>机器狗音效</h1>
  <p>点一下就会在 S3 上播放对应的本地音效。</p>
</header>
<div id="status">正在连接…</div>
<div class="chat">
  <input id="chatText" type="text" maxlength="200" placeholder="输入一句话，让它开口回复"/>
  <button id="chatSend" type="button">发送</button>
</div>
<section class="transcript">
  <div class="label">识别到的内容</div>
  <div id="sttLatest" class="stt-text">还没有识别结果。对板子说话后，这里会显示云端识别到的文字。</div>
  <div id="turns" class="turns"></div>
</section>
<div class="grid" id="grid"></div>
<script>
const statusEl = document.getElementById('status');
const grid = document.getElementById('grid');
const sttLatest = document.getElementById('sttLatest');
const turnsEl = document.getElementById('turns');
let transcriptCount = 0;
function addTurn(event, text) {
  if (!text) return;
  const div = document.createElement('div');
  div.className = 'turn ' + event;
  const tag = document.createElement('span');
  tag.textContent = event === 'stt' ? '识别' : '回复';
  const body = document.createElement('span');
  body.textContent = text;
  div.appendChild(tag);
  div.appendChild(body);
  turnsEl.appendChild(div);
  turnsEl.scrollTop = turnsEl.scrollHeight;
  if (event === 'stt') sttLatest.textContent = text;
}
async function refreshTranscript() {
  try {
    const data = await fetch('/api/transcript').then(r => r.json());
    const events = data.events || [];
    for (let i = transcriptCount; i < events.length; i++) {
      addTurn(events[i].event, events[i].text);
    }
    transcriptCount = events.length;
  } catch (e) {}
}
async function load() {
  const hello = await fetch('/api/hello').then(r => r.json());
  statusEl.textContent = hello.ip ? ('已连接  ' + hello.ip) : '已连接';
  for (const item of hello.sfx) {
    const b = document.createElement('button');
    b.textContent = item.label;
    b.onclick = async () => {
      b.classList.add('busy');
      statusEl.textContent = '正在播放：' + item.label;
      try {
        const res = await fetch('/api/play?id=' + encodeURIComponent(item.id), {method:'POST'});
        const data = await res.json();
        if (data.event === 'error') statusEl.textContent = data.message || '播放失败';
        else statusEl.textContent = '正在播放：' + item.label;
      } catch (e) {
        statusEl.textContent = '发送失败，请检查连接';
      }
      b.classList.remove('busy');
    };
    grid.appendChild(b);
  }
}
async function sendChat() {
  const input = document.getElementById('chatText');
  const text = (input.value || '').trim();
  if (!text) return;
  statusEl.textContent = '已发送：' + text + '，等待开口回复…';
  try {
    const res = await fetch('/api/chat', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({text})});
    const data = await res.json();
    if (data.event === 'error') statusEl.textContent = data.message || '发送失败';
  } catch (e) {
    statusEl.textContent = '发送失败，请检查连接和 Wi-Fi';
  }
}
document.getElementById('chatSend').onclick = sendChat;
document.getElementById('chatText').addEventListener('keydown', (e) => { if (e.key === 'Enter') sendChat(); });
load().catch(() => { statusEl.textContent = '无法连上板子，请确认同一局域网'; });
refreshTranscript();
setInterval(refreshTranscript, 1000);
</script>
</body>
</html>
)HTML";


constexpr size_t kTranscriptMax = 20;

struct TranscriptItem {
    std::string event;
    std::string text;
};

std::mutex g_transcript_mu;
std::vector<TranscriptItem> g_transcript;

void RememberTranscript(const char* event, const char* text) {
    std::lock_guard<std::mutex> lock(g_transcript_mu);
    g_transcript.push_back({event ? event : "", text ? text : ""});
    if (g_transcript.size() > kTranscriptMax) {
        g_transcript.erase(g_transcript.begin(), g_transcript.end() - kTranscriptMax);
    }
}

}  // namespace


void SfxControl::NotifyText(const char* event, const char* text) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event ? event : "");
    cJSON_AddStringToObject(root, "text", text ? text : "");
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed != nullptr) {
        RememberTranscript(event, text);
        ESP_LOGI(TAG, "S3CTRL %s", printed);
        UsbLink::GetInstance().SendCtrlJson(printed);
        cJSON_free(printed);
    }
}

std::string TranscriptJsonLocked() {
    cJSON* arr = cJSON_CreateArray();
    for (const auto& item : g_transcript) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "event", item.event.c_str());
        cJSON_AddStringToObject(obj, "text", item.text.c_str());
        cJSON_AddItemToArray(arr, obj);
    }
    char* printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    std::string json = printed ? printed : "[]";
    if (printed) {
        cJSON_free(printed);
    }
    return json;
}

static esp_err_t HandleTranscript(httpd_req_t* req) {
    std::string events;
    {
        std::lock_guard<std::mutex> lock(g_transcript_mu);
        events = TranscriptJsonLocked();
    }
    std::string body = std::string("{") + "\"events\":" + events + "}";
    return SendJson(req, body.c_str());
}

void SfxControl::Reply(const char* event, const char* id, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event", event ? event : "");
    if (id != nullptr) {
        cJSON_AddStringToObject(root, "id", id);
    }
    if (message != nullptr) {
        cJSON_AddStringToObject(root, "message", message);
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text != nullptr) {
        ESP_LOGI(TAG, "S3CTRL %s", text);
        UsbLink::GetInstance().SendCtrlJson(text);
        cJSON_free(text);
    }
}

bool SfxControl::Play(const char* id) {
    const SfxEntry* entry = FindSfx(id);
    if (entry == nullptr) {
        Reply("error", id, "unknown sfx id");
        return false;
    }
    auto sound = entry->sound();
    if (sound.empty()) {
        Reply("error", id, "sfx asset missing");
        return false;
    }
    Application::GetInstance().PlaySound(sound);
    Reply("playing", entry->id, entry->label);
    return true;
}

void SfxControl::HandleLine(const char* line) {
    if (line == nullptr) {
        return;
    }
    while (*line == ' ' || *line == '\t' || *line == '\r') {
        ++line;
    }
    if (*line != '{') {
        return;
    }
    cJSON* root = cJSON_Parse(line);
    if (root == nullptr) {
        Reply("error", nullptr, "invalid json");
        return;
    }
    const cJSON* cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cJSON_IsString(cmd) || cmd->valuestring == nullptr) {
        cJSON_Delete(root);
        Reply("error", nullptr, "missing cmd");
        return;
    }
    const char* name = cmd->valuestring;
    if (strcmp(name, "play") == 0) {
        const cJSON* id = cJSON_GetObjectItem(root, "id");
        Play(cJSON_IsString(id) ? id->valuestring : nullptr);
    } else if (strcmp(name, "hello") == 0 || strcmp(name, "list") == 0) {
        std::string ip = GetStaIp();
        cJSON* hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "event", "hello");
        cJSON_AddStringToObject(hello, "ip", ip.c_str());
        cJSON_AddNumberToObject(hello, "port", SFX_HTTP_PORT);
        cJSON* arr = cJSON_AddArrayToObject(hello, "sfx");
        for (const auto& entry : kSfx) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(entry.id));
        }
        char* text = cJSON_PrintUnformatted(hello);
        cJSON_Delete(hello);
        if (text != nullptr) {
            ESP_LOGI(TAG, "S3CTRL %s", text);
        UsbLink::GetInstance().SendCtrlJson(text);
            cJSON_free(text);
        }
    } else if (strcmp(name, "chat") == 0 || strcmp(name, "say") == 0) {
        const cJSON* text = cJSON_GetObjectItem(root, "text");
        if (!cJSON_IsString(text) || text->valuestring == nullptr || text->valuestring[0] == '\0') {
            Reply("error", nullptr, "missing text");
        } else {
            Application::GetInstance().InvokeTextChat(text->valuestring);
            Reply("chat", nullptr, text->valuestring);
        }
    } else if (strcmp(name, "demo") == 0) {
        Application::GetInstance().Schedule([]() { Application::GetInstance().PlaySfxDemo(); });
        Reply("playing", "demo", "sfx demo");
    } else if (strcmp(name, "abort") == 0) {
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);
        Reply("abort", nullptr, "requested");
    } else if (strcmp(name, "listen") == 0) {
        Application::GetInstance().StartListening();
        Reply("listen", nullptr, "requested");
    } else if (strcmp(name, "close_audio") == 0 || strcmp(name, "stop_listen") == 0) {
        Application::GetInstance().StopListening();
        Reply(name, nullptr, "requested");
    } else if (strcmp(name, "set_ota") == 0) {
        const cJSON* url = cJSON_GetObjectItem(root, "url");
        std::string value;
        if (cJSON_IsString(url) && url->valuestring != nullptr) {
            value = url->valuestring;
        }
        if (value.empty() || value == "official") {
#ifdef CONFIG_OTA_URL
            value = CONFIG_OTA_URL;
#else
            value = "https://api.tenclass.net/xiaozhi/ota/";
#endif
        }
        {
            Settings settings("wifi", true);
            settings.SetString("ota_url", value);
        }
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "event", "ota");
        cJSON_AddStringToObject(resp, "url", value.c_str());
        cJSON_AddBoolToObject(resp, "reboot_required", true);
        cJSON_AddStringToObject(resp, "message", "ota_url saved, reboot to apply");
        char* printed = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (printed != nullptr) {
            ESP_LOGI(TAG, "S3CTRL %s", printed);
        UsbLink::GetInstance().SendCtrlJson(printed);
            cJSON_free(printed);
        }
    } else if (strcmp(name, "open_audio") == 0) {
        Application::GetInstance().EnsureAudioChannelIdle();
        Reply("open_audio", nullptr, "requested");
    } else {
        Reply("error", nullptr, "unknown cmd");
    }
    cJSON_Delete(root);
}

void SfxControl::StartSerialTask() {
    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<SfxControl*>(arg);
            char line[512];
            size_t n = 0;
            ESP_LOGI(TAG, "USB serial command reader started");
            while (true) {
#if SFX_HAS_USJ_LL
                uint8_t buf[64];
                int got = usb_serial_jtag_ll_read_rxfifo(buf, sizeof(buf));
                if (got <= 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                for (int i = 0; i < got; ++i) {
                    char c = static_cast<char>(buf[i]);
                    if (c == '\n' || c == '\r') {
                        if (n > 0) {
                            line[n] = '\0';
                            self->HandleLine(line);
                            n = 0;
                        }
                        continue;
                    }
                    if (n + 1 < sizeof(line)) {
                        line[n++] = c;
                    } else {
                        n = 0;
                    }
                }
#else
                vTaskDelay(pdMS_TO_TICKS(1000));
#endif
            }
        },
        "sfx_serial", 4096, this, 5, nullptr);
}

static void BuildSfxCatalogJson(char* out, size_t out_size) {
    size_t used = 0;
    auto append = [&](const char* s) {
        size_t len = strlen(s);
        if (used + len + 1 < out_size) {
            memcpy(out + used, s, len);
            used += len;
            out[used] = '\0';
        }
    };
    append("[");
    for (size_t i = 0; i < sizeof(kSfx) / sizeof(kSfx[0]); ++i) {
        if (i) append(",");
        append("{\"id\":\"");
        append(kSfx[i].id);
        append("\",\"label\":\"");
        append(kSfx[i].label);
        append("\"}");
    }
    append("]");
}

static esp_err_t HandleIndex(httpd_req_t* req) {
    AddCors(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t HandleHello(httpd_req_t* req) {
    std::string ip = GetStaIp();
    std::string body = "{\"event\":\"hello\",\"ip\":\"";
    body += ip;
    body += "\",\"port\":";
    body += std::to_string(SFX_HTTP_PORT);
    body += ",\"sfx\":";
    body += SfxControl::GetInstance().ListJson();
    body += "}";
    return SendJson(req, body.c_str());
}

static esp_err_t HandlePlay(httpd_req_t* req) {
    AddCors(req);
    char id[64] = {0};
    char query[128] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "id", id, sizeof(id));
    }
    if (id[0] == '\0' && req->content_len > 0 && req->content_len < 256) {
        char buf[256];
        int received = httpd_req_recv(req, buf, req->content_len);
        if (received > 0) {
            buf[received] = '\0';
            cJSON* root = cJSON_Parse(buf);
            if (root) {
                const cJSON* item = cJSON_GetObjectItem(root, "id");
                if (cJSON_IsString(item) && item->valuestring) {
                    strncpy(id, item->valuestring, sizeof(id) - 1);
                }
                cJSON_Delete(root);
            }
        }
    }
    bool ok = SfxControl::GetInstance().Play(id[0] ? id : nullptr);
    if (!ok) {
        httpd_resp_set_status(req, "404 Not Found");
        return SendJson(req, "{\"event\":\"error\",\"message\":\"unknown sfx id\"}");
    }
    std::string body = "{\"event\":\"playing\",\"id\":\"";
    body += id;
    body += "\"}";
    return SendJson(req, body.c_str());
}

static esp_err_t HandleChat(httpd_req_t* req) {
    AddCors(req);
    char text[512] = {0};
    char query[256] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "text", text, sizeof(text));
    }
    if (text[0] == '\0' && req->content_len > 0 && req->content_len < 512) {
        char buf[512];
        int received = httpd_req_recv(req, buf, req->content_len);
        if (received > 0) {
            buf[received] = '\0';
            cJSON* root = cJSON_Parse(buf);
            if (root) {
                const cJSON* item = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(item) && item->valuestring) {
                    strncpy(text, item->valuestring, sizeof(text) - 1);
                }
                cJSON_Delete(root);
            }
        }
    }
    if (text[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return SendJson(req, "{\"event\":\"error\",\"message\":\"missing text\"}");
    }
    Application::GetInstance().InvokeTextChat(text);
    std::string body = "{\"event\":\"chat\",\"text\":\"";
    body += text;
    body += "\"}";
    return SendJson(req, body.c_str());
}

static constexpr size_t kMaxSpeakBytes = 256 * 1024;

static esp_err_t HandleSpeak(httpd_req_t* req) {
    AddCors(req);
    int total = req->content_len;
    if (total <= 0 || total > static_cast<int>(kMaxSpeakBytes)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return SendJson(req, "{\"event\":\"error\",\"message\":\"missing or too large audio\"}");
    }
    std::string ogg;
    ogg.resize(static_cast<size_t>(total));
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, ogg.data() + received, total - received);
        if (n <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return SendJson(req, "{\"event\":\"error\",\"message\":\"audio recv failed\"}");
        }
        received += n;
    }
    if (ogg.size() < 4 || ogg.compare(0, 4, "OggS") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return SendJson(req, "{\"event\":\"error\",\"message\":\"audio must be Ogg Opus\"}");
    }
    Application::GetInstance().Schedule([ogg = std::move(ogg)]() {
        Application::GetInstance().PlaySound(std::string_view(ogg.data(), ogg.size()));
    });
    ESP_LOGI(TAG, "S3CTRL {\"event\":\"speaking\",\"bytes\":%d}", received);
    return SendJson(req, "{\"event\":\"speaking\"}");
}

static esp_err_t HandleCmd(httpd_req_t* req) {
    AddCors(req);
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_set_status(req, "400 Bad Request");
        return SendJson(req, "{\"event\":\"error\",\"message\":\"missing or too large json\"}");
    }
    std::string body;
    body.resize(static_cast<size_t>(total));
    int received = 0;
    while (received < total) {
        int n = httpd_req_recv(req, body.data() + received, total - received);
        if (n <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return SendJson(req, "{\"event\":\"error\",\"message\":\"json recv failed\"}");
        }
        received += n;
    }
    SfxControl::GetInstance().HandleLine(body.c_str());
    return SendJson(req, "{\"event\":\"accepted\"}");
}

static esp_err_t HandleOptions(httpd_req_t* req) {
    AddCors(req);
    return httpd_resp_send(req, "", 0);
}

void SfxControl::StartHttpServer() {
    if (http_started_) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = SFX_HTTP_PORT;
    config.ctrl_port = 32770;
    config.lru_purge_enable = true;
    config.stack_size = 12288;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = nullptr;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGW(TAG, "HTTP server start failed, will retry");
        return;
    }
    http_started_ = true;

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleIndex,
        .user_ctx = nullptr,
    };
    const httpd_uri_t hello_uri = {
        .uri = "/api/hello",
        .method = HTTP_GET,
        .handler = HandleHello,
        .user_ctx = nullptr,
    };
    const httpd_uri_t play_get_uri = {
        .uri = "/api/play",
        .method = HTTP_GET,
        .handler = HandlePlay,
        .user_ctx = nullptr,
    };
    const httpd_uri_t play_post_uri = {
        .uri = "/api/play",
        .method = HTTP_POST,
        .handler = HandlePlay,
        .user_ctx = nullptr,
    };
    const httpd_uri_t chat_get_uri = {
        .uri = "/api/chat",
        .method = HTTP_GET,
        .handler = HandleChat,
        .user_ctx = nullptr,
    };
    const httpd_uri_t chat_post_uri = {
        .uri = "/api/chat",
        .method = HTTP_POST,
        .handler = HandleChat,
        .user_ctx = nullptr,
    };
    const httpd_uri_t transcript_uri = {
        .uri = "/api/transcript",
        .method = HTTP_GET,
        .handler = HandleTranscript,
        .user_ctx = nullptr,
    };
    const httpd_uri_t speak_post_uri = {
        .uri = "/api/speak",
        .method = HTTP_POST,
        .handler = HandleSpeak,
        .user_ctx = nullptr,
    };
    const httpd_uri_t cmd_post_uri = {
        .uri = "/api/cmd",
        .method = HTTP_POST,
        .handler = HandleCmd,
        .user_ctx = nullptr,
    };
    const httpd_uri_t options_uri = {
        .uri = "/api/*",
        .method = HTTP_OPTIONS,
        .handler = HandleOptions,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &hello_uri);
    httpd_register_uri_handler(server, &play_get_uri);
    httpd_register_uri_handler(server, &play_post_uri);
    httpd_register_uri_handler(server, &chat_get_uri);
    httpd_register_uri_handler(server, &chat_post_uri);
    httpd_register_uri_handler(server, &transcript_uri);
    httpd_register_uri_handler(server, &speak_post_uri);
    httpd_register_uri_handler(server, &cmd_post_uri);
    httpd_register_uri_handler(server, &options_uri);
    ESP_LOGI(TAG, "HTTP control server started on port %d", SFX_HTTP_PORT);
}

void SfxControl::Start() {
    if (started_) {
        return;
    }
    started_ = true;
    BuildSfxCatalogJson(list_json_, sizeof(list_json_));
    UsbLink::GetInstance().EnsureStarted();
    if (true) {
        return;
    }
    StartSerialTask();

    xTaskCreate(
        [](void* arg) {
            auto* self = static_cast<SfxControl*>(arg);
            std::string last_ip;
            for (int i = 0; i < 120 && !self->http_started_; ++i) {
                self->StartHttpServer();
                if (!self->http_started_) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            while (true) {
                std::string ip = GetStaIp();
                if (!ip.empty() && ip != last_ip) {
                    last_ip = ip;
                    ESP_LOGI(TAG, "S3CTRL {\"event\":\"ready\",\"ip\":\"%s\",\"port\":%d}", ip.c_str(),
                             SFX_HTTP_PORT);
                    ESP_LOGI(TAG, "Open http://%s:%d to click-play SFX", ip.c_str(), SFX_HTTP_PORT);
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        },
        "sfx_http", 6144, this, 5, nullptr);
}
