#pragma once

#include <cstdio>

/* Define DEBUG at build time (e.g. -DDEBUG) to enable verbose gRPC proxy logging.
 * Do NOT define it unconditionally in production builds to avoid leaking
 * internal state (app IDs, function names, gRPC operations) to stderr. */

#if !defined(LOG) && defined(DEBUG)
#define LOG(fmt, ...) do { fprintf(stderr, "gRPC:proxy:%s() " fmt, __func__, ##__VA_ARGS__); } while (0)
#else
#define LOG(fmt, ...) do {} while (0)
#endif
