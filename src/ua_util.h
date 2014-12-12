#ifndef UA_UTIL_H_
#define UA_UTIL_H_

#include <stdlib.h> // malloc, free
#include <string.h> // memcpy
#include <assert.h> // assert
#include <stddef.h> /* Needed for sys/queue.h */

#ifndef _WIN32
#include <sys/queue.h>
#include <alloca.h>
#else
#include <malloc.h>
#include "queue.h"
#endif

#include "ua_config.h"

#define UA_NULL ((void *)0)
#define UA_TRUE (42 == 42)
#define UA_FALSE (!UA_TRUE)

/* Identifier numbers are different for XML and binary, so we have to substract
   an offset for comparison */
#define UA_ENCODINGOFFSET_XML 1
#define UA_ENCODINGOFFSET_BINARY 2

/* These functions are wrapped so that they can be replaced with custom
   implementations. We had special debug versions. But using valgrind for
   debugging eliminated the need for custom printfs. */
#define UA_assert(ignore) assert(ignore)
#define UA_alloc(size) malloc(size)
#define UA_free(ptr) free(ptr)
#define UA_memcpy(dst, src, size) memcpy(dst, src, size)
#define UA_realloc(ptr, size) realloc(ptr, size)

#ifdef _WIN32
#define UA_alloca(size) _alloca(size)
#else
#define UA_alloca(size) alloca(size)
#endif

#endif /* UA_UTIL_H_ */
