#pragma once

#include <cstdio>
#include <ctime>

#define LOG_INFO(fmt, ...)                                                          \
  do {                                                                              \
    struct timespec _ts;                                                            \
    clock_gettime(CLOCK_REALTIME, &_ts);                                            \
    struct tm _tm;                                                                  \
    localtime_r(&_ts.tv_sec, &_tm);                                                \
    fprintf(stdout, "[INFO  %02d:%02d:%02d.%03ld] " fmt "\n",                       \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, _ts.tv_nsec / 1000000,            \
            ##__VA_ARGS__);                                                         \
    fflush(stdout);                                                                 \
  } while (0)

#define LOG_WARN(fmt, ...)                                                          \
  do {                                                                              \
    struct timespec _ts;                                                            \
    clock_gettime(CLOCK_REALTIME, &_ts);                                            \
    struct tm _tm;                                                                  \
    localtime_r(&_ts.tv_sec, &_tm);                                                \
    fprintf(stderr, "[WARN  %02d:%02d:%02d.%03ld] " fmt "\n",                       \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, _ts.tv_nsec / 1000000,            \
            ##__VA_ARGS__);                                                         \
    fflush(stderr);                                                                 \
  } while (0)

#define LOG_ERROR(fmt, ...)                                                         \
  do {                                                                              \
    struct timespec _ts;                                                            \
    clock_gettime(CLOCK_REALTIME, &_ts);                                            \
    struct tm _tm;                                                                  \
    localtime_r(&_ts.tv_sec, &_tm);                                                \
    fprintf(stderr, "[ERROR %02d:%02d:%02d.%03ld] " fmt "\n",                       \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, _ts.tv_nsec / 1000000,            \
            ##__VA_ARGS__);                                                         \
    fflush(stderr);                                                                 \
  } while (0)

#define LOG_FATAL(fmt, ...)                                                         \
  do {                                                                              \
    struct timespec _ts;                                                            \
    clock_gettime(CLOCK_REALTIME, &_ts);                                            \
    struct tm _tm;                                                                  \
    localtime_r(&_ts.tv_sec, &_tm);                                                \
    fprintf(stderr, "[FATAL %02d:%02d:%02d.%03ld] " fmt "\n",                       \
            _tm.tm_hour, _tm.tm_min, _tm.tm_sec, _ts.tv_nsec / 1000000,            \
            ##__VA_ARGS__);                                                         \
    fflush(stderr);                                                                 \
  } while (0)
