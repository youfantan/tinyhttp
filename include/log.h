#pragma once

#include <io.h>
#include <evchannel.h>
#include <memory.h>

#include <chrono>
#include <fstream>
#include <unistd.h>

#define INFO(x) log(log_level::INFO, x, __FILE__, __LINE__);
#define WARN(x) log(log_level::WARN, x, __FILE__, __LINE__);
#define ERROR(x) log(log_level::ERROR, x, __FILE__, __LINE__);
#define FATAL(x) log(log_level::FATAL, x, __FILE__, __LINE__);
#define DEBUG(x) log(log_level::DEBUG, x, __FILE__, __LINE__);
#define TRACE(x) log(log_level::TRACE, x, __FILE__, __LINE__);

constexpr static std::string COMMON = "\033[39m";
constexpr static std::string RED = "\033[31m";
constexpr static std::string YELLOW = "\033[33m";
constexpr static std::string GREEN = "\033[32m";

class event_channel;
static std::ofstream log_output;

/* 通过传入time_point<system_clock>(默认值为调用时间)对时间戳进行格式化 */
inline std::string get_formatted_time(const std::string_view& format, const std::chrono::time_point<std::chrono::system_clock>& tp = std::chrono::system_clock::now()) {
    const auto t = std::chrono::system_clock::to_time_t(tp);
    const auto lt = localtime(&t);
    /* 在这里我的IDE(CLion 2024.1 EAP (Nova))给出了错误提示，但实际上这行代码通过了编译，运行也没有任何问题。疑似是IDE的问题。*/
    // return std::vformat(format, std::make_format_args(lt));
    /* 使用C库提供的函数进行格式化 */
    char buffer[64];
    strftime(buffer, sizeof(buffer), format.data(), lt);
    return {buffer};
}

enum class log_level {
    UNKNOWN = 0,
    TRACE,
    INFO,
    WARN,
    DEBUG,
    ERROR,
    FATAL
};

inline std::string get_log_level(const log_level& level) {
    switch (level) {
        case log_level::TRACE: return "TRACE";
        case log_level::INFO: return "INFO";
        case log_level::WARN: return "WARN";
        case log_level::DEBUG: return "DEBUG";
        case log_level::ERROR: return "ERROR";
        case log_level::FATAL: return "FATAL";
        case log_level::UNKNOWN: return "UNKNOWNLVL";
        default: return "NOSUCHLVL";
    }
}

/* 日志事件 */
class event_log {
private:
    general_shared_array_buffer_t buffer_;
    log_level lv_;
    time_t tm_;
    std::string_view from_;
    std::string_view msg_;
public:
    constexpr static int unique_event_id = 1;
    event_log(const log_level& lv, const time_t& tm, const std::string_view& from, const std::string_view& msg) : lv_(lv), tm_(tm), from_(from), msg_(msg), buffer_(sizeof(lv_) + sizeof(tm_) + from.length() +  msg.length() + 1, new heap_allocator()) {
        buffer_stream stream(buffer_);
        stream.set_auto_expand(true);
        if (!stream.append(lv_)) {
            throw memory_exception("event_log::event_log()", "cannot write log_level to event_log array buffer");
        }
        if (!stream.append(tm_)) {
            throw memory_exception("event_log::event_log()", "cannot write time_t to event_log array buffer");
        }
        if (!stream.append(from_)) {
            throw memory_exception("event_log::event_log()", "cannot write from_process to event_log array buffer");
        }
        if (!stream.append(msg_)) {
            throw memory_exception("event_log::event_log()", "cannot write message to event_log array buffer");
        }
    }
    explicit event_log(general_shared_array_buffer_t buffer) : buffer_(std::move(buffer)) {
        buffer_stream stream(buffer_);
        try {
            lv_ = stream.get_as<log_level>();
            tm_ = stream.get_as<time_t>();
        } catch (memory_exception&) {
            throw;
        }
        from_ = stream.get_as();
        if (from_.empty()) {
            throw memory_exception("event_log::event_log()", "cannot read from_process from event_log array buffer");
        }
        msg_ = stream.get_as();
        if (msg_.empty()) {
            throw memory_exception("event_log::event_log()", "cannot read message from event_log array buffer");
        }
    }
    [[nodiscard]] general_shared_array_buffer_t content() const {return buffer_;}
    [[nodiscard]] auto get_log_level() const {
        return lv_;
    }
    [[nodiscard]] auto get_time() const {
        return std::chrono::system_clock::from_time_t(tm_);
    }
    [[nodiscard]] auto get_from() const {
        return from_;
    }
    [[nodiscard]] auto get_msg() const {
        return msg_;
    }
};

static event_channel* pevchannel;

/* 初始化日志模块，可能抛出io_exception */
static void log_init(event_channel& evchannel) {
    pevchannel = &evchannel;
    try {
        check_path_exists("logs/", std::filesystem::file_type::directory);
    } catch (io_exception&) {
        throw;
    }
    auto now = std::chrono::system_clock::now();
    std::filesystem::path log_file_path(std::format("logs/{}.log", get_formatted_time("%Y-%m-%d-%H-%M-%S")));
    log_output.open(log_file_path, std::ios::out | std::ios::binary);
    if(!log_output) {
        throw io_exception("log_init()", std::format("cannot not open {}", log_file_path.c_str()));
    }
    //在事件总线里注册日志事件
    evchannel.subscribe<event_log>([&](const event_log& e) {
        auto lv = get_log_level(e.get_log_level());
        auto tm = get_formatted_time("%Y-%m-%d %H:%M:%S", e.get_time());
        std::string formatted_log_msg = std::format("[{}][{}][{}] {}\n", lv, tm, e.get_from(), e.get_msg());
        log_output.write(formatted_log_msg.c_str(), static_cast<ssize_t>(formatted_log_msg.length()));
#ifdef ENABLE_ANSI_DISPLAY
        switch (e.get_log_level()) {
            case log_level::DEBUG: fwrite(GREEN.c_str(), 1, GREEN.length(), stdout);
            case log_level::WARN: fwrite(YELLOW.c_str(), 1, YELLOW.length(), stdout);
            case log_level::ERROR: fwrite(RED.c_str(), 1, RED.length(), stdout);
            case log_level::FATAL: fwrite(RED.c_str(), 1 ,RED.length(), stdout);
            default: fwrite(COMMON.c_str(), 1 ,COMMON.length(), stdout);
        }
#endif
        fwrite(formatted_log_msg.c_str(), 1, formatted_log_msg.length(), stdout);
#ifdef ENABLE_ANSI_DISPLAY
        fwrite(COMMON.c_str(), 1 ,COMMON.length(), stdout);
#endif
    });
}

static void log_start(event_channel& event_channel) {
    pevchannel = &event_channel;
}

static bool check_log_exists() {
    if (pevchannel == nullptr) {
        return false;
    }
    return true;
}

static void log(log_level level, const std::string_view& msg, const char* file, int line) {
    std::string from = std::format("{}({}:{})", getpid(), file, line);
    if (pevchannel == nullptr) {
        return;
    }
    time_t tm;
    time(&tm);
    pevchannel->post(event_log(level, tm, from, msg));

}

inline void log_close() {
    log_output.close();
}