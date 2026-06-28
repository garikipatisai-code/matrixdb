// CPU test for OB-1 structured logging: leveled logger with a runtime threshold. Tests the filter
// (enabled()) and the actual stderr emission (by redirecting cerr's rdbuf).
#include "logging.hpp"
#include <cassert>
#include <sstream>
#include <string>
#include <iostream>

static void test_level_filter() {
    Log::set_level(LogLevel::WARN);                            // default
    assert(!Log::enabled(LogLevel::DEBUG) && !Log::enabled(LogLevel::INFO) && "below threshold -> filtered");
    assert(Log::enabled(LogLevel::WARN) && Log::enabled(LogLevel::ERROR) && "at/above threshold -> enabled");
    Log::set_level(LogLevel::DEBUG);                           // verbose
    assert(Log::enabled(LogLevel::DEBUG) && Log::get_level() == LogLevel::DEBUG && "DEBUG threshold -> all enabled");
    Log::set_level(LogLevel::ERROR);                           // quiet
    assert(!Log::enabled(LogLevel::WARN) && Log::enabled(LogLevel::ERROR) && "ERROR threshold -> only ERROR");
    std::cout << "[level filter] ok\n";
}

static void test_emission() {
    // capture cerr
    std::ostringstream cap; std::streambuf* old = std::cerr.rdbuf(cap.rdbuf());
    Log::set_level(LogLevel::WARN);
    Log::emit(LogLevel::DEBUG, "debug-suppressed");            // below threshold -> nothing
    Log::emit(LogLevel::INFO,  "info-suppressed");             // below threshold -> nothing
    Log::emit(LogLevel::WARN,  "a warning");                   // emitted
    Log::emit(LogLevel::ERROR, "an error");                    // emitted
    std::cerr.rdbuf(old);                                      // restore
    const std::string out = cap.str();
    assert(out.find("debug-suppressed") == std::string::npos && "below-threshold suppressed");
    assert(out.find("info-suppressed") == std::string::npos && "below-threshold suppressed");
    assert(out.find("[WARN] a warning") != std::string::npos && "WARN emitted with prefix");
    assert(out.find("[ERROR] an error") != std::string::npos && "ERROR emitted with prefix");
    Log::set_level(LogLevel::WARN);                            // restore default for other tests/links
    std::cout << "[emission] ok\n";
}

int main() { test_level_filter(); test_emission(); std::cout << "ALL LOGGING TESTS PASSED\n"; return 0; }
