#pragma once

#include <string>
#include <pthread.h>
#include <cstdarg>

enum LogLevel {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_TELEMETRY = 4
};

class Logger {
public:
    static Logger& instance();

    void init(const std::string& log_file_path = "", LogLevel min_level = LOG_INFO);
    void log(LogLevel level, const char* module_tag, const char* format, ...);
    void set_min_level(LogLevel level);
    void enable_console(bool enable);
    void set_ipc_mode(bool enable);

private:
    Logger();
    ~Logger();

    pthread_mutex_t mutex_;
    LogLevel min_level_ = LOG_INFO;
    bool console_enabled_ = true;
    bool ipc_mode_ = false;
    std::string file_path_ = "";
    int file_fd_ = -1;
};

#define LOG_D(tag, fmt, ...) Logger::instance().log(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) Logger::instance().log(LOG_INFO, tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) Logger::instance().log(LOG_WARN, tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) Logger::instance().log(LOG_ERROR, tag, fmt, ##__VA_ARGS__)
