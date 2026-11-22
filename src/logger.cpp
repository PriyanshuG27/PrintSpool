#include "logger.h"
#include "clock.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

Logger::Logger() {
    pthread_mutex_init(&mutex_, nullptr);
}

Logger::~Logger() {
    pthread_mutex_destroy(&mutex_);
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const std::string& log_file_path, LogLevel min_level) {
    pthread_mutex_lock(&mutex_);
    min_level_ = min_level;
    file_path_ = log_file_path;
    pthread_mutex_unlock(&mutex_);
}

void Logger::set_min_level(LogLevel level) {
    pthread_mutex_lock(&mutex_);
    min_level_ = level;
    pthread_mutex_unlock(&mutex_);
}

void Logger::enable_console(bool enable) {
    pthread_mutex_lock(&mutex_);
    console_enabled_ = enable;
    pthread_mutex_unlock(&mutex_);
}

void Logger::set_ipc_mode(bool enable) {
    pthread_mutex_lock(&mutex_);
    ipc_mode_ = enable;
    pthread_mutex_unlock(&mutex_);
}

void Logger::log(LogLevel level, const char* module_tag, const char* format, ...) {
    pthread_mutex_lock(&mutex_);
    if (level < min_level_) {
        pthread_mutex_unlock(&mutex_);
        return;
    }

    double t = SimClock::instance().now();
    const char* lvl_str = "INFO";
    const char* color_code = "\033[0m";

    switch (level) {
        case LOG_DEBUG:     lvl_str = "DEBUG"; color_code = "\033[36m"; break;
        case LOG_INFO:      lvl_str = "INFO "; color_code = "\033[32m"; break;
        case LOG_WARN:      lvl_str = "WARN "; color_code = "\033[33m"; break;
        case LOG_ERROR:     lvl_str = "ERROR"; color_code = "\033[31;1m"; break;
        case LOG_TELEMETRY: lvl_str = "TELEM"; color_code = "\033[35m"; break;
    }

    char msg_buf[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(msg_buf, sizeof(msg_buf), format, args);
    va_end(args);

    if (ipc_mode_) {
        std::string esc;
        esc.reserve(std::strlen(msg_buf) + 16);
        for (const char* p = msg_buf; *p; ++p) {
            if (*p == '"') esc += "\\\"";
            else if (*p == '\\') esc += "\\\\";
            else if (*p == '\n') esc += "\\n";
            else if (*p == '\r') esc += "\\r";
            else if (*p == '\t') esc += "\\t";
            else esc += *p;
        }
        std::printf("{\"type\":\"log\",\"time\":%.2f,\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}\n",
                    t, lvl_str, module_tag, esc.c_str());
        std::fflush(stdout);
    } else if (console_enabled_) {
        std::printf("%s[T=%7.2fs] [%s] [%-13s]\033[0m %s\n",
                    color_code, t, lvl_str, module_tag, msg_buf);
        std::fflush(stdout);
    }

    pthread_mutex_unlock(&mutex_);
}
