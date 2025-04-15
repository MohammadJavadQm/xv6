#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "custom_logger.h"

// تابعی برای چاپ سطح لاگ به صورت متن
static const char* level_to_string(log_level_t level) {
    switch (level) {
        case INFO:  return "INFO";
        case WARN:  return "WARNING";
        case ERROR: return "ERROR";
        default:    return "UNKNOWN";
    }
}

// پیاده‌سازی تابع لاگر
void log_message(log_level_t level, const char *message) {
    printf("%s − %s\n", level_to_string(level), message);
}
