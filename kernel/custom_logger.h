#ifndef CUSTOM_LOGGER_H
#define CUSTOM_LOGGER_H

// تعریف سطوح لاگ به صورت enum
typedef enum {
    INFO,
    WARN,
    ERROR
} log_level_t;

// پروتوتایپ تابع اصلی لاگر
void log_message(log_level_t level, const char *message);

#endif // CUSTOM_LOGGER_H
