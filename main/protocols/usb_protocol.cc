#include "usb_protocol.h"
#include "application.h"
#include "audio_service.h"
#include "board.h"
#include "sfx_control.h"
#include "system_info.h"

#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <esp_log.h>
#include <freertos/task.h>
#if __has_include("hal/usb_serial_jtag_ll.h")
#include "hal/usb_serial_jtag_ll.h"
#define HAS_USJ_LL_FLUSH 1
#else
#define HAS_USJ_LL_FLUSH 0
#endif

#include <cstring>
#include <vector>

#define TAG "UsbProto"

namespace {
constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;
constexpr uint8_t kTypeJson = 0x01;
constexpr uint8_t kTypeAudio = 0x02;
constexpr uint8_t kTypeCtrl = 0x03;
constexpr size_t kMaxPayload = 4096;

UsbProtocol* g_usb_proto = nullptr;

uint8_t ComputeXorCrc(uint8_t msg_type, uint8_t len_hi, uint8_t len_lo, const uint8_t* payload, size_t len) {
    uint8_t acc = msg_type ^ len_hi ^ len_lo;
    for (size_t i = 0; i < len; ++i) {
        acc ^= payload[i];
    }
    return acc;
}
}  // namespace

void UsbLink::EnsureStarted() {
    if (started_) {
        return;
    }
    started_ = true;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {
            .tx_buffer_size = 16384,
            .rx_buffer_size = 16384,
        };
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %d", (int)err);
        }
    }
    xTaskCreate(
        [](void* arg) {
            static_cast<UsbLink*>(arg)->RxTask();
            vTaskDelete(nullptr);
        },
        "usb_link_rx", 8192, this, 8, nullptr);
}

void UsbLink::AttachProtocol(UsbProtocol* proto) {
    std::lock_guard<std::mutex> lock(proto_mutex_);
    proto_ = proto;
}

bool UsbLink::SendFrame(uint8_t msg_type, const uint8_t* payload, size_t len) {
    if (len > kMaxPayload) {
        return false;
    }
    EnsureStarted();
    uint8_t len_hi = static_cast<uint8_t>((len >> 8) & 0xFF);
    uint8_t len_lo = static_cast<uint8_t>(len & 0xFF);
    uint8_t crc = ComputeXorCrc(msg_type, len_hi, len_lo, payload, len);

    uint8_t stack_buf[256];
    std::vector<uint8_t> heap_buf;
    uint8_t* out = nullptr;
    const size_t total = 6 + len;
    if (total <= sizeof(stack_buf)) {
        out = stack_buf;
    } else {
        heap_buf.resize(total);
        out = heap_buf.data();
    }
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = msg_type;
    out[3] = len_hi;
    out[4] = len_lo;
    out[5] = crc;
    if (len > 0 && payload != nullptr) {
        memcpy(out + 6, payload, len);
    }

    std::lock_guard<std::mutex> lock(tx_mutex_);
    size_t sent = 0;
    while (sent < total) {
        int written = usb_serial_jtag_write_bytes(out + sent, total - sent, pdMS_TO_TICKS(15));
        if (written <= 0) {
            return false;
        }
        sent += static_cast<size_t>(written);
    }
#if HAS_USJ_LL_FLUSH
    usb_serial_jtag_ll_txfifo_flush();
#endif
    return true;
}

bool UsbLink::SendCtrlJson(const char* json_str) {
    if (json_str == nullptr) {
        return false;
    }
    return SendFrame(kTypeCtrl, reinterpret_cast<const uint8_t*>(json_str), strlen(json_str));
}

void UsbLink::RxTask() {
    std::vector<uint8_t> buf;
    buf.reserve(4096);
    uint8_t chunk[512];

    while (true) {
        int got = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (got <= 0) {
            continue;
        }
        buf.insert(buf.end(), chunk, chunk + got);

        while (true) {
            size_t idx = 0;
            bool found = false;
            for (; idx + 1 < buf.size(); ++idx) {
                if (buf[idx] == kMagic0 && buf[idx + 1] == kMagic1) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (buf.size() > 1) {
                    uint8_t last = buf.back();
                    buf.clear();
                    if (last == kMagic0) {
                        buf.push_back(last);
                    }
                }
                break;
            }
            if (idx > 0) {
                buf.erase(buf.begin(), buf.begin() + idx);
            }
            if (buf.size() < 6) {
                break;
            }
            uint8_t msg_type = buf[2];
            uint16_t length = (static_cast<uint16_t>(buf[3]) << 8) | static_cast<uint16_t>(buf[4]);
            uint8_t crc = buf[5];
            if ((msg_type != kTypeJson && msg_type != kTypeAudio && msg_type != kTypeCtrl) || length > kMaxPayload) {
                buf.erase(buf.begin(), buf.begin() + 2);
                continue;
            }
            if (buf.size() < 6 + length) {
                break;
            }
            const uint8_t* payload = buf.data() + 6;
            if (ComputeXorCrc(msg_type, buf[3], buf[4], payload, length) != crc) {
                buf.erase(buf.begin(), buf.begin() + 2);
                continue;
            }
            std::vector<uint8_t> copy(payload, payload + length);
            buf.erase(buf.begin(), buf.begin() + 6 + length);

            UsbProtocol* proto = nullptr;
            {
                std::lock_guard<std::mutex> lock(proto_mutex_);
                proto = proto_;
            }

            if (msg_type == kTypeAudio) {
                if (proto != nullptr) {
                    proto->HandleIncomingAudio(copy.data(), copy.size());
                }
            } else if (msg_type == kTypeJson) {
                if (proto != nullptr) {
                    proto->HandleIncomingJson(reinterpret_cast<const char*>(copy.data()), copy.size());
                }
            } else if (msg_type == kTypeCtrl) {
                std::string line(reinterpret_cast<const char*>(copy.data()), copy.size());
                cJSON* root = cJSON_Parse(line.c_str());
                const cJSON* cmd = root ? cJSON_GetObjectItem(root, "cmd") : nullptr;
                if (cJSON_IsString(cmd) && cmd->valuestring && strcmp(cmd->valuestring, "usb_attach") == 0) {
                    cJSON_Delete(root);
                    auto& app = Application::GetInstance();
                    auto st = app.GetDeviceState();
                    if (st == kDeviceStateSpeaking) {
                        app.AbortSpeaking(kAbortReasonNone);
                    } else if (st == kDeviceStateListening) {
                        app.StopListening();
                    }
                    if (proto != nullptr) {
                        proto->OnHostAttach();
                    }
                    SendCtrlJson("{\"event\":\"usb_attached\"}");
                } else {
                    if (root) {
                        cJSON_Delete(root);
                    }
                    SfxControl::GetInstance().HandleLine(line.c_str());
                }
            }
        }
    }
}

UsbProtocol::UsbProtocol() {
    event_group_handle_ = xEventGroupCreate();
    server_sample_rate_ = 24000;
    server_frame_duration_ = OPUS_FRAME_DURATION_MS;
    g_usb_proto = this;
}

UsbProtocol::~UsbProtocol() {
    UsbLink::GetInstance().AttachProtocol(nullptr);
    if (g_usb_proto == this) {
        g_usb_proto = nullptr;
    }
    if (event_group_handle_ != nullptr) {
        vEventGroupDelete(event_group_handle_);
    }
}

UsbProtocol* UsbProtocol::Instance() {
    return g_usb_proto;
}

bool UsbProtocol::Start() {
    UsbLink::GetInstance().EnsureStarted();
    UsbLink::GetInstance().AttachProtocol(this);
    opened_ = true;
    error_occurred_ = false;
    SendHello();
    return true;
}

void UsbProtocol::OnHostAttach() {
    opened_ = true;
    error_occurred_ = false;
    server_hello_ok_ = false;
    xEventGroupClearBits(event_group_handle_, USB_PROTOCOL_SERVER_HELLO_EVENT);
    SendHello();
}

bool UsbProtocol::OpenAudioChannel() {
    opened_ = true;
    error_occurred_ = false;
    if (!server_hello_ok_) {
        SendHello();
        xEventGroupWaitBits(
            event_group_handle_,
            USB_PROTOCOL_SERVER_HELLO_EVENT,
            pdTRUE,
            pdFALSE,
            pdMS_TO_TICKS(1500));
    }
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

void UsbProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

bool UsbProtocol::IsAudioChannelOpened() const {
    return opened_ && server_hello_ok_ && !error_occurred_;
}

bool UsbProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!opened_ || !packet || packet->payload.empty()) {
        return false;
    }
    return UsbLink::GetInstance().SendFrame(kTypeAudio, packet->payload.data(), packet->payload.size());
}

bool UsbProtocol::SendText(const std::string& text) {
    if (!opened_ || text.empty()) {
        return false;
    }
    return UsbLink::GetInstance().SendFrame(
        kTypeJson, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

bool UsbProtocol::SendHello() {
    return SendText(GetHelloMessage());
}

std::string UsbProtocol::GetHelloMessage() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    AddTextFontCapabilities(root);
    cJSON_AddStringToObject(root, "transport", "usb");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    char* json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str ? json_str : "{}");
    if (json_str) {
        cJSON_free(json_str);
    }
    cJSON_Delete(root);
    return message;
}

void UsbProtocol::ParseServerHello(const cJSON* root) {
    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id) && session_id->valuestring) {
        session_id_ = session_id->valuestring;
    }
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }
    server_hello_ok_ = true;
    opened_ = true;
    xEventGroupSetBits(event_group_handle_, USB_PROTOCOL_SERVER_HELLO_EVENT);
    if (on_connected_ != nullptr) {
        on_connected_();
    }
    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
}

void UsbProtocol::HandleIncomingJson(const char* data, size_t len) {
    last_incoming_time_ = std::chrono::steady_clock::now();
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        return;
    }
    const cJSON* type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type) && type->valuestring != nullptr) {
        if (strcmp(type->valuestring, "hello") == 0) {
            ParseServerHello(root);
        } else if (strcmp(type->valuestring, "ping") == 0) {
            SendText("{\"type\":\"pong\"}");
        } else if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
    }
    cJSON_Delete(root);
}

void UsbProtocol::HandleIncomingAudio(const uint8_t* data, size_t len) {
    last_incoming_time_ = std::chrono::steady_clock::now();
    if (on_incoming_audio_ == nullptr || len == 0) {
        return;
    }
    auto pkt = std::make_unique<AudioStreamPacket>(AudioStreamPacket{
        .sample_rate = server_sample_rate_,
        .frame_duration = server_frame_duration_,
        .timestamp = 0,
        .payload = std::vector<uint8_t>(data, data + len),
    });
    on_incoming_audio_(std::move(pkt));
}
