#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
#include <stddef.h>
typedef uint32_t TickType_t;
typedef uint32_t StackType_t;
typedef uint32_t UBaseType_t;
typedef void *TaskHandle_t;
size_t xPortGetFreeHeapSize(void);
size_t xPortGetMinimumEverFreeHeapSize(void);
#define configTICK_RATE_HZ 1000U
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#endif
