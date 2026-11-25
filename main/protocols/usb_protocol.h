#ifndef USB_PROTOCOL_H
#define USB_PROTOCOL_H

#include "protocol.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <string>

#define USB_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class UsbProtocol;

class UsbLink {
public:
    static UsbLink& GetInstance() {
        static UsbLink instance;
        return instance;
    }

    void EnsureStarted();
    void AttachProtocol(UsbProtocol* proto);
    bool SendFrame(uint8_t msg_type, const uint8_t* payload, size_t len);
    bool SendCtrlJson(const char* json_str);

private:
    UsbLink() = default;
    void RxTask();

    std::mutex tx_mutex_;
    std::mutex proto_mutex_;
    UsbProtocol* proto_ = nullptr;
    bool started_ = false;
};

class UsbProtocol : public Protocol {
public:
    UsbProtocol();
    ~UsbProtocol() override;

    static UsbProtocol* Instance();

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    void OnHostAttach();
    void HandleIncomingJson(const char* data, size_t len);
    void HandleIncomingAudio(const uint8_t* data, size_t len);

protected:
    bool SendText(const std::string& text) override;
    bool IsTimeout() const override { return false; }

private:
    std::string GetHelloMessage();
    void ParseServerHello(const cJSON* root);
    bool SendHello();

    EventGroupHandle_t event_group_handle_ = nullptr;
    bool opened_ = false;
    bool server_hello_ok_ = false;
    int version_ = 1;
};

#endif  // USB_PROTOCOL_H
