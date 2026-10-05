#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "lwip/sys.h"

struct host_sem { pthread_mutex_t lock; pthread_cond_t ready; unsigned count; };
struct host_mbox { pthread_mutex_t lock; pthread_cond_t ready, space; void **items; unsigned cap, head, count; };
static pthread_mutex_t protection;
static pthread_once_t once = PTHREAD_ONCE_INIT;
_Atomic unsigned host_live_sems, host_live_mboxes, host_duplicate_signals;
static void init_protection(void) {
  pthread_mutexattr_t a; assert(!pthread_mutexattr_init(&a));
  assert(!pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE));
  assert(!pthread_mutex_init(&protection, &a)); pthread_mutexattr_destroy(&a);
}
void sys_init(void) { pthread_once(&once, init_protection); }
sys_prot_t sys_arch_protect(void) { sys_init(); assert(!pthread_mutex_lock(&protection)); return 0; }
void sys_arch_unprotect(sys_prot_t p) { (void)p; assert(!pthread_mutex_unlock(&protection)); }
u32_t sys_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (u32_t)((uint64_t)t.tv_sec*1000 + t.tv_nsec/1000000); }
u32_t sys_jiffies(void) { return sys_now(); }
static struct timespec deadline(u32_t ms) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += ms/1000; t.tv_nsec += (ms%1000)*1000000; if(t.tv_nsec>=1000000000){t.tv_sec++;t.tv_nsec-=1000000000;} return t; }
err_t sys_sem_new(sys_sem_t *s,u8_t n) { *s=calloc(1,sizeof(**s)); assert(*s); pthread_mutex_init(&(*s)->lock,NULL); pthread_cond_init(&(*s)->ready,NULL); (*s)->count=n; atomic_fetch_add(&host_live_sems,1); return ERR_OK; }
void sys_sem_signal(sys_sem_t *s) { assert(s&&*s); pthread_mutex_lock(&(*s)->lock); if((*s)->count) atomic_fetch_add(&host_duplicate_signals,1); (*s)->count++; pthread_cond_signal(&(*s)->ready); pthread_mutex_unlock(&(*s)->lock); }
u32_t sys_arch_sem_wait(sys_sem_t *s,u32_t ms) { u32_t start=sys_now(); struct timespec d=deadline(ms); int e=0; assert(s&&*s); pthread_mutex_lock(&(*s)->lock); while(!(*s)->count && !e) e=ms?pthread_cond_timedwait(&(*s)->ready,&(*s)->lock,&d):pthread_cond_wait(&(*s)->ready,&(*s)->lock); if(e==ETIMEDOUT){pthread_mutex_unlock(&(*s)->lock);return SYS_ARCH_TIMEOUT;} assert(!e); (*s)->count--; pthread_mutex_unlock(&(*s)->lock); return sys_now()-start; }
void sys_sem_free(sys_sem_t *s) { assert(s&&*s); assert(!(*s)->count); pthread_cond_destroy(&(*s)->ready); pthread_mutex_destroy(&(*s)->lock); free(*s); *s=NULL; atomic_fetch_sub(&host_live_sems,1); }
int sys_sem_valid(sys_sem_t *s){return s&&*s;}
void sys_sem_set_invalid(sys_sem_t *s){*s=NULL;}
err_t sys_mutex_new(sys_mutex_t *m){*m=malloc(sizeof(**m));assert(*m);pthread_mutex_init(*m,NULL);return ERR_OK;}
void sys_mutex_lock(sys_mutex_t *m){pthread_mutex_lock(*m);}
void sys_mutex_unlock(sys_mutex_t *m){pthread_mutex_unlock(*m);}
void sys_mutex_free(sys_mutex_t *m){pthread_mutex_destroy(*m);free(*m);*m=NULL;}
int sys_mutex_valid(sys_mutex_t *m){return m&&*m;}
void sys_mutex_set_invalid(sys_mutex_t *m){*m=NULL;}
err_t sys_mbox_new(sys_mbox_t *m,int size){*m=calloc(1,sizeof(**m));assert(*m);(*m)->cap=size>0?(unsigned)size:16;(*m)->items=calloc((*m)->cap,sizeof(void*));assert((*m)->items);pthread_mutex_init(&(*m)->lock,NULL);pthread_cond_init(&(*m)->ready,NULL);pthread_cond_init(&(*m)->space,NULL);atomic_fetch_add(&host_live_mboxes,1);return ERR_OK;}
void sys_mbox_post(sys_mbox_t *m,void *v){pthread_mutex_lock(&(*m)->lock);while((*m)->count==(*m)->cap)pthread_cond_wait(&(*m)->space,&(*m)->lock);(*m)->items[((*m)->head+(*m)->count)%(*m)->cap]=v;(*m)->count++;pthread_cond_signal(&(*m)->ready);pthread_mutex_unlock(&(*m)->lock);}
err_t sys_mbox_trypost(sys_mbox_t *m,void *v){err_t e=ERR_OK;pthread_mutex_lock(&(*m)->lock);if((*m)->count==(*m)->cap)e=ERR_MEM;else{(*m)->items[((*m)->head+(*m)->count)%(*m)->cap]=v;(*m)->count++;pthread_cond_signal(&(*m)->ready);}pthread_mutex_unlock(&(*m)->lock);return e;}
u32_t sys_arch_mbox_fetch(sys_mbox_t *m,void **v,u32_t ms){u32_t start=sys_now();struct timespec d=deadline(ms);int e=0;pthread_mutex_lock(&(*m)->lock);while(!(*m)->count&&!e)e=ms?pthread_cond_timedwait(&(*m)->ready,&(*m)->lock,&d):pthread_cond_wait(&(*m)->ready,&(*m)->lock);if(e==ETIMEDOUT){pthread_mutex_unlock(&(*m)->lock);return SYS_ARCH_TIMEOUT;}assert(!e);if(v)*v=(*m)->items[(*m)->head];(*m)->head=((*m)->head+1)%(*m)->cap;(*m)->count--;pthread_cond_signal(&(*m)->space);pthread_mutex_unlock(&(*m)->lock);return sys_now()-start;}
u32_t sys_arch_mbox_tryfetch(sys_mbox_t *m,void **v){pthread_mutex_lock(&(*m)->lock);if(!(*m)->count){pthread_mutex_unlock(&(*m)->lock);return SYS_MBOX_EMPTY;}if(v)*v=(*m)->items[(*m)->head];(*m)->head=((*m)->head+1)%(*m)->cap;(*m)->count--;pthread_cond_signal(&(*m)->space);pthread_mutex_unlock(&(*m)->lock);return 0;}
void sys_mbox_free(sys_mbox_t *m){assert(m&&*m);assert(!(*m)->count);pthread_cond_destroy(&(*m)->ready);pthread_cond_destroy(&(*m)->space);pthread_mutex_destroy(&(*m)->lock);free((*m)->items);free(*m);*m=NULL;atomic_fetch_sub(&host_live_mboxes,1);}
int sys_mbox_valid(sys_mbox_t *m){return m&&*m;}
void sys_mbox_set_invalid(sys_mbox_t *m){*m=NULL;}
struct thread_start{lwip_thread_fn fn;void *arg;};
static void *thread_entry(void *p){struct thread_start s=*(struct thread_start*)p;free(p);s.fn(s.arg);return NULL;}
sys_thread_t sys_thread_new(const char *name,lwip_thread_fn fn,void *arg,int stack,int prio){(void)name;(void)stack;(void)prio;pthread_t t;struct thread_start *s=malloc(sizeof(*s));assert(s);s->fn=fn;s->arg=arg;assert(!pthread_create(&t,NULL,thread_entry,s));pthread_detach(t);return t;}
sys_thread_t sys_thread_new_tcm(const char *name,lwip_thread_fn fn,void *arg,int stack,int prio){return sys_thread_new(name,fn,arg,stack,prio);}
