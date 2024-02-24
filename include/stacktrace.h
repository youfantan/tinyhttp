#pragma once

#include <string>
#include <execinfo.h>
#include <sstream>

constexpr static int max_backtraces = 128;
/* 返回当前线程的堆栈追踪字符串 */
static std::string get_stack_trace() {
    void* addrs[max_backtraces];
    int frames = backtrace(addrs, max_backtraces);
    char** sym = backtrace_symbols(addrs, frames);
    if (sym != nullptr) {
        std::stringstream ss;
        //避免输出本函数的追踪信息
        for (int i = 1; i < frames; ++i) {
            ss << sym[i] << std::endl;
        }
        free(sym);
        return ss.str();
    }
    return {};
}
