#pragma once
// OB-1 structured logging: a tiny leveled logger with a runtime-settable threshold, so operators can
// filter/route diagnostics instead of grepping unconditional `cout`. Levels DEBUG<INFO<WARN<ERROR;
// default WARN (quiet — only problems surface). Writes "[LEVEL] msg\n" to stderr (logs are not data — keeps
// stdout clean). One global threshold (the engine is single-owner; a real deployment swaps the sink for a
// structured/JSON appender + per-subsystem levels).
#include <iostream>
#include <string>

enum class LogLevel : int { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Log {
public:
    static void set_level(LogLevel l) { level() = l; }
    static LogLevel get_level() { return level(); }
    // True if a message at `l` passes the current threshold (would be emitted). Lets callers skip building
    // an expensive message — and lets tests assert the filter without capturing stderr.
    static bool enabled(LogLevel l) { return static_cast<int>(l) >= static_cast<int>(level()); }
    static void emit(LogLevel l, const std::string& msg) {
        if (!enabled(l)) return;
        std::cerr << prefix(l) << msg << '\n';
    }
private:
    static LogLevel& level() { static LogLevel lv = LogLevel::WARN; return lv; }
    static const char* prefix(LogLevel l) {
        switch (l) {
            case LogLevel::DEBUG: return "[DEBUG] ";
            case LogLevel::INFO:  return "[INFO] ";
            case LogLevel::WARN:  return "[WARN] ";
            default:              return "[ERROR] ";
        }
    }
};
