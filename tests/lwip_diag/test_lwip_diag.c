#include <sys/time.h>
#include <assert.h>
#include <stdatomic.h>
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/stats.h"
/* Compile the actual source once; expose its static socket table only to tests. */
#include LWIP_SOCKETS_SOURCE
extern _Atomic unsigned host_live_sems,host_live_mboxes,host_duplicate_signals;

struct snapshot {
  unsigned used[MEMP_MAX];
  unsigned mem_used, sems, mboxes, active, bound, timewait;
};
struct command { int fd, op; struct snapshot snapshot; };
enum { SNAPSHOT, ZERO_WINDOW, CHECK_QUEUED, CHECK_LASTDATA, CHECK_NULL, BUSY, IDLE };
static void ready(void *arg){sys_sem_signal(arg);}
static void command_callback(void *arg) {
  struct command *c=arg;
  struct lwip_sock *s=c->op==SNAPSHOT?NULL:get_socket(c->fd);
  struct tcp_pcb *p=s?s->conn->pcb.tcp:NULL;
  if(c->op!=SNAPSHOT) assert(s);
  switch(c->op) {
    case SNAPSHOT:
      for(int i=0;i<MEMP_MAX;i++) c->snapshot.used[i]=lwip_stats.memp[i]->used;
      c->snapshot.mem_used=lwip_stats.mem.used;
      c->snapshot.sems=atomic_load(&host_live_sems);
      c->snapshot.mboxes=atomic_load(&host_live_mboxes);
      for(p=tcp_active_pcbs;p;p=p->next)c->snapshot.active++;
      for(p=tcp_bound_pcbs;p;p=p->next)c->snapshot.bound++;
      for(p=tcp_tw_pcbs;p;p=p->next)c->snapshot.timewait++;
      break;
    case ZERO_WINDOW: assert(p&&p->state==ESTABLISHED); assert(!p->unacked&&!p->unsent); p->snd_wnd=0; break;
    case CHECK_QUEUED: assert(p&&p->unsent&&p->snd_queuelen>0); assert(p->unsent->p); assert(p->unsent->len==511); break;
    case CHECK_LASTDATA: assert(s->lastdata); break;
    case CHECK_NULL: assert(!p); break;
    case BUSY: assert(s->conn->state==NETCONN_NONE); s->conn->state=NETCONN_WRITE; break;
    case IDLE: assert(s->conn->state==NETCONN_WRITE); s->conn->state=NETCONN_NONE; break;
    default: abort();
  }
}
struct sync_command { struct command *cmd; sys_sem_t sem; };
static void sync_callback(void *arg){struct sync_command *c=arg;command_callback(c->cmd);sys_sem_signal(&c->sem);}
static void run_command(struct command *cmd) {
  struct sync_command c={.cmd=cmd}; assert(sys_sem_new(&c.sem,0)==ERR_OK);
  assert(tcpip_send_msg_wait_sem(sync_callback,&c,&c.sem)==ERR_OK);
  sys_sem_free(&c.sem);
}
static void operation(int fd,int op){struct command c={.fd=fd,.op=op};run_command(&c);}
static struct snapshot take_snapshot(void) {
  /* Drain loopback callbacks posted by TCP resets before accounting. */
  struct command c={.op=SNAPSHOT};
  for(int i=0;i<3;i++){memset(&c.snapshot,0,sizeof(c.snapshot));run_command(&c);}
  c.snapshot.sems--; /* Snapshot's own synchronous command semaphore. */
  return c.snapshot;
}
static void same_resources(struct snapshot *a,struct snapshot *b) {
  for(int i=0;i<MEMP_MAX;i++) {
    /* TCP starts/stops one lazy timer; it expires after the last abort. */
    if(i==MEMP_SYS_TIMEOUT){assert(b->used[i]<=a->used[i]+1);continue;}
    if(a->used[i]!=b->used[i])fprintf(stderr,"pool %d: %u -> %u\n",i,a->used[i],b->used[i]);
    assert(a->used[i]==b->used[i]);
  }
  assert(a->mem_used==b->mem_used);
  assert(a->sems==b->sems); assert(a->mboxes==b->mboxes);
  assert(a->active==b->active); assert(a->bound==b->bound);
  assert(a->timewait==b->timewait); assert(b->timewait==0);
  assert(atomic_load(&host_duplicate_signals)==0);
}
static void clean_against(struct snapshot *base){struct snapshot now=take_snapshot();same_resources(base,&now);}
static struct sockaddr_in address(unsigned port) {
  struct sockaddr_in a={0};a.sin_len=sizeof(a);a.sin_family=AF_INET;
  a.sin_port=lwip_htons(port);a.sin_addr.s_addr=lwip_htonl(0x7f000001);return a;
}
static void pair(int listener,unsigned port,int *sender,int *receiver) {
  *sender=lwip_socket(AF_INET,SOCK_STREAM,0);assert(*sender>=0);
  struct sockaddr_in a=address(port);assert(lwip_connect(*sender,(struct sockaddr*)&a,sizeof(a))==0);
  *receiver=lwip_accept(listener,NULL,NULL);assert(*receiver>=0);
  assert(lwip_diag_can_send(*sender)==1);
}
int main(void) {
  setvbuf(stdout,NULL,_IONBF,0);
  sys_sem_t sem;sys_init();assert(sys_sem_new(&sem,0)==ERR_OK);
  tcpip_init(ready,&sem);sys_sem_wait(&sem);sys_sem_free(&sem);
  struct snapshot empty=take_snapshot();
  int listener=lwip_socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
  struct sockaddr_in a=address(18081);
  assert(lwip_bind(listener,(struct sockaddr*)&a,sizeof(a))==0);
  assert(lwip_listen(listener,2)==0);
  struct snapshot base=take_snapshot();
  puts("real SDK lwIP startup and loopback listener OK");

  /* UDP and listening sockets reject operations and retain caller ownership. */
  int udp=lwip_socket(AF_INET,SOCK_DGRAM,0);assert(udp>=0);
  assert(lwip_diag_can_send(udp)==-1);assert(errno==EOPNOTSUPP);
  assert(lwip_abortclose(udp)==-1);assert(errno==EOPNOTSUPP);
  assert(get_socket(udp));assert(lwip_close(udp)==0);
  assert(lwip_diag_can_send(listener)==-1);assert(errno==EINPROGRESS);
  assert(lwip_abortclose(listener)==-1);assert(errno==EINPROGRESS);
  assert(get_socket(listener));clean_against(&base);
  puts("UDP/listener rejection preserves ownership: PASS");

  int first_sender=-1,first_receiver=-1;
  char line[511];memset(line,'D',sizeof(line));
  for(int cycle=0;cycle<100;cycle++) {
    int sender,receiver;pair(listener,18081,&sender,&receiver);
    if(cycle==0){first_sender=sender;first_receiver=receiver;}
    assert(sender==first_sender&&receiver==first_receiver);
    operation(sender,ZERO_WINDOW);
    assert(lwip_send(sender,line,sizeof(line),MSG_DONTWAIT)==sizeof(line));
    operation(sender,CHECK_QUEUED);
    assert(lwip_diag_can_send(sender)==0);
    for(int retry=0;retry<3;retry++)assert(lwip_diag_can_send(sender)==0);
    assert(lwip_abortclose(sender)==0);
    assert(!tryget_socket(sender));
    operation(receiver,CHECK_NULL); /* The real loopback reset cleared peer pcb. */
    assert(lwip_diag_can_send(receiver)==-1);
    assert(lwip_abortclose(receiver)==0);
    assert(!tryget_socket(receiver));
    clean_against(&base);
  }
  puts("100 zero-window sends / aborts / reset peers / fd reuse / resource restoration: PASS");

  /* A partial socket receive owns a pbuf through lastdata; abort must free it. */
  int sender,receiver;pair(listener,18081,&sender,&receiver);
  assert(lwip_send(receiver,line,sizeof(line),MSG_DONTWAIT)==sizeof(line));
  char byte;assert(lwip_recv(sender,&byte,1,0)==1);assert(byte=='D');
  operation(sender,CHECK_LASTDATA);
  operation(sender,ZERO_WINDOW);
  assert(lwip_send(sender,line,sizeof(line),MSG_DONTWAIT)==sizeof(line));
  operation(sender,CHECK_QUEUED);
  assert(lwip_abortclose(sender)==0);assert(lwip_abortclose(receiver)==0);
  clean_against(&base);
  puts("partial receive lastdata and pending transmit buffers freed: PASS");

  /* Busy netconn leaves the fd intact; restoring idle permits cleanup. */
  pair(listener,18081,&sender,&receiver);operation(sender,BUSY);
  assert(lwip_diag_can_send(sender)==-1);assert(errno==EINPROGRESS);
  assert(lwip_abortclose(sender)==-1);assert(errno==EINPROGRESS);
  assert(get_socket(sender));operation(sender,IDLE);
  assert(lwip_abortclose(sender)==0);assert(lwip_abortclose(receiver)==0);
  clean_against(&base);puts("busy netconn rejection retains socket until retry: PASS");

  assert(lwip_close(listener)==0);sys_msleep(300);clean_against(&empty);
  struct snapshot final=take_snapshot();assert(final.used[MEMP_SYS_TIMEOUT]==empty.used[MEMP_SYS_TIMEOUT]);
  assert(lwip_abortclose(-1)==-1);assert(errno==EBADF);
  assert(lwip_diag_can_send(-1)==-1);assert(errno==EBADF);
  puts("ASAN/UBSAN, all pool/heap counts, semaphore balance, no TIME_WAIT: PASS");
  return 0;
}
