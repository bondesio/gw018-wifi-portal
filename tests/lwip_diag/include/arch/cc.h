#ifndef HOST_CC_H
#define HOST_CC_H
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { fprintf(stderr, "lwIP assertion %s at %s:%d\n", x, __FILE__, __LINE__); abort(); } while (0)
#define LWIP_RAND() ((unsigned int)rand())
typedef unsigned int UBaseType_t;
static inline UBaseType_t uxTaskPriorityGet(void *task) { (void)task; return 0; }
static inline void vTaskPrioritySet(void *task, UBaseType_t prio) { (void)task; (void)prio; }
static inline unsigned int pmu_get_wakelock_status(void) { return 0; }
#endif
