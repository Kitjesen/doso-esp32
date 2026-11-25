#pragma once

class SfxControl {
public:
    static SfxControl& GetInstance() {
        static SfxControl instance;
        return instance;
    }

    // Start USB-serial command reader and the local HTTP control server.
    void Start();

    // Play a baked-in clip by short id, e.g. "obstacle". Returns false if unknown.
    bool Play(const char* id);

    // JSON array of known ids, e.g. ["obstacle","wake"].
    const char* ListJson() const { return list_json_; }

    // Handle one JSON command object or a line like {"cmd":"play","id":"obstacle"}.
    // Replies are logged with the prefix S3CTRL so the PC can parse them.
    void HandleLine(const char* line);

    // Forward cloud STT/TTS text to the PC over the S3CTRL serial line.
    void NotifyText(const char* event, const char* text);

private:
    SfxControl() = default;
    SfxControl(const SfxControl&) = delete;
    SfxControl& operator=(const SfxControl&) = delete;

    void StartSerialTask();
    void StartHttpServer();
    void Reply(const char* event, const char* id, const char* message = nullptr);

    bool started_ = false;
    bool http_started_ = false;
    char list_json_[2048]{};
};
