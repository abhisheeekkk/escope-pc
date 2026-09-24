#pragma once
#include "hal/IDataSource.h"
#include <thread>
#include <atomic>
#include <string>

namespace escope {

class StmDataSource : public IDataSource {
public:
    explicit StmDataSource(std::string port = "");
    ~StmDataSource() override;

    std::vector<DeviceInfo> enumerate()                       override;
    SourceStatus            open(const std::string& id = "") override;
    void                    close()                           override;
    bool                    is_open()    const override { return fd_ >= 0; }
    SourceStatus            configure(const CaptureSession&)  override { return SourceStatus::OK; }
    SourceStatus            start(CaptureSession& session)    override;
    void                    stop()                            override;
    bool                    is_running() const override { return running_; }
    std::string             name()       const override { return "STM32 EmbeddedScope"; }

    void set_data_callback(DataCallback cb)       override { data_cb_    = std::move(cb); }
    void set_error_callback(ErrorCallback cb)     override { error_cb_   = std::move(cb); }
    void set_trigger_callback(TriggerCallback cb) override { trigger_cb_ = std::move(cb); }

private:
    void reader_loop(CaptureSession* session);
    bool open_port(const std::string& path);

    std::string       port_;
    int               fd_      = -1;
    std::atomic<bool> running_ {false};
    std::thread       worker_;

    DataCallback      data_cb_;
    ErrorCallback     error_cb_;
    TriggerCallback   trigger_cb_;
};

} // namespace escope
