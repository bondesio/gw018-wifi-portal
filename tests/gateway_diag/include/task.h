#include "FreeRTOS.h"
void test_enter(void);
void test_exit(void);
#define taskENTER_CRITICAL() test_enter()
#define taskEXIT_CRITICAL() test_exit()
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
int xTaskCreate(void (*function)(void *), const char *name, unsigned int stack,
                void *parameter, unsigned int priority, void *handle);

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
