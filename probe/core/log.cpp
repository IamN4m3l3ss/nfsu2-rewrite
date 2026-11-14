// Core logging: forwarded to the loader, which owns the log file and ring so they survive reloads.
#include "probe.h"

#include <cstdarg>
#include <cstdio>

namespace probe {

void logf(const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (has_api()) api().log(msg);
}

json log_tail(size_t n, uint64_t since) {
    return json::parse(api().log_tail_json(static_cast<uint32_t>(n), since));
}

}  // namespace probe
