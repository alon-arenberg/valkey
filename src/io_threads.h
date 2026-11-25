#ifndef IO_THREADS_H
#define IO_THREADS_H

#include "server.h"

/* Tag values for tagged pointers on work/response queues.
 * Tags must fit in 3 bits (0-7) due to 8-byte alignment from jemalloc
 * with --with-lg-quantum=3. SPSC and SPMC tags are separate enums
 * because they occupy independent queues and may reuse values. */

/* Tags for the SPSC private inbox (main thread → specific I/O thread). */
typedef enum {
    JOB_SPSC_FREE_ARGV = 0,
    JOB_SPSC_POLL = 1,
    JOB_SPSC_ACCEPT = 2,
} JobRequestSPSC;

/* Tags for the SPMC shared inbox (main thread → any I/O thread). */
typedef enum {
    JOB_REQ_READ_CLIENT = 0,
    JOB_REQ_WRITE_CLIENT,
    JOB_REQ_FREE_OBJ,
    JOB_REQ_POLL,
    JOB_REQ_ACCEPT,
    JOB_REQ_CLUSTER_READ,
    JOB_REQ_CLUSTER_WRITE,
    JOB_REQ_CLUSTER_ACCEPT,
    JOB_REQ_COUNT
} JobRequestSPMC;
static_assert(JOB_REQ_COUNT <= 8, "JOB_REQ_COUNT must not exceed 8 for pointer arithmetic");

/* Tags for the MPSC response queue (I/O threads → main thread). */
typedef enum {
    JOB_RES_READ_CLIENT = 0,
    JOB_RES_WRITE_CLIENT,
    JOB_RES_CLUSTER_READ,
    JOB_RES_CLUSTER_WRITE,
    JOB_RES_CLUSTER_ACCEPT,
    JOB_RES_ACCEPT_SYSCALL,
    JOB_RES_COUNT
} JobResult;
static_assert(JOB_RES_COUNT <= 8, "JOB_RES_COUNT must not exceed 8 for pointer arithmetic");

/* Accept offload: one job per listening socket.
 *
 * The worker drains the socket's backlog into results[] with accept4() and
 * does the per-socket setup that touches nothing shared. The main thread turns
 * each result into a client, because admission control, createClient() and
 * connAccept() all mutate server state.
 *
 * results[] is a fixed array so the job needs no allocation per event. A
 * backlog deeper than the cap is not lost: epoll is level triggered, so
 * re-arming the listener after a saturated batch refires immediately. */
#define IO_ACCEPT_BATCH_MAX 32

typedef struct acceptIOResult {
    int fd;                  /* the accepted socket */
    int port;                /* peer port, 0 for unix sockets */
    char ip[NET_IP_STR_LEN]; /* peer address, empty for unix sockets */
    connection *conn;        /* built by the worker; may be in an error state */
    client *client;          /* pre-built by the worker, NULL if it did not */
} acceptIOResult;

typedef struct acceptIOJob {
    /* Main thread owns these. Stable while the job is in flight. */
    connListener *listener;
    int listen_fd;
    void *accept_priv;    /* passed to ct->conn_create_accepted() */
    int unix_socket;      /* 1: no peer address, no keepalive */
    int max_per_cycle;    /* accept() attempts allowed for one job */
    int client_headroom;  /* clients the worker may pre-build for this job */
    int in_flight;        /* queued or running; cleared when the response lands */
    int discard;          /* listener closed under the job: drop its response */

    /* Worker writes these, main thread reads them after `done` is set or after
     * the response is dequeued. */
    _Atomic int done;
    int accepted;
    int saturated; /* hit the batch cap, the backlog may hold more */
    int err_errno; /* errno that ended the accept loop, 0 if none */
    acceptIOResult results[IO_ACCEPT_BATCH_MAX];
} acceptIOJob;

typedef void (*job_handler)(void *);

void initIOThreads(int prev_threads_num);
void killIOThreads(void);
int inMainThread(void);
int trySendReadToIOThreads(client *c);
int trySendWriteToIOThreads(client *c);
int tryOffloadFreeObjToIOThreads(robj *o);
int tryOffloadFreeArgvToIOThreads(client *c, int argc, robj **argv);
void IOThreadsAfterSleep(int numevents);
void IOThreadsBeforeSleep(long long current_time);
void drainIOThreadsQueue(void);
void testOnlyInitIOThreadQueues(void);
void testOnlyFreeIOThreadQueues(void);
void testOnlyFillIOThreadInbox(void);
size_t testOnlyGetClusterIOPendingResponses(void);
const void *testOnlyGetIOThreadActiveTimeSlot(int id);
const void *testOnlyGetIOJobsFinishedCounter(void);
const void *testOnlyGetIOJobsSubmittedCounter(void);
void trySendPollJobToIOThreads(void);
int trySendAcceptToIOThreads(connection *conn);
int trySendAcceptSyscallToIOThreads(connListener *listener,
                                    int listen_fd,
                                    void *accept_priv,
                                    int unix_socket,
                                    int max_per_cycle);
void cancelListenerAcceptOffload(connListener *listener);
struct clusterLink;
int trySendClusterReadToIOThreads(struct clusterLink *link);
int trySendClusterWriteToIOThreads(struct clusterLink *link);
int trySendClusterAcceptToIOThreads(connection *conn);
int updateIOThreads(const char **err);
long long getIOThreadActiveTimeMicroseconds(int id);
int clientHasPendingIO(struct client *c);
int processIOThreadsResponses(void);
int getCurTid(void);
void sendToMainThread(void *data, int type);

#endif /* IO_THREADS_H */
