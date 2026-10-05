#ifndef HOST_SYS_ARCH_H
#define HOST_SYS_ARCH_H
#include <pthread.h>
typedef struct host_sem *sys_sem_t;
typedef struct host_mbox *sys_mbox_t;
typedef pthread_mutex_t *sys_mutex_t;
typedef pthread_t sys_thread_t;
typedef unsigned int sys_prot_t;
#define SYS_SEM_NULL NULL
#define SYS_MBOX_NULL NULL
#endif
