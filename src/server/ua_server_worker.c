#include "ua_server_internal.h"

/**
 * Timed Work
 *
 * Timed work comes in two flavors: timedWork and repeatedWork
 *
 */

/** The timed event and repeated event are only relevant for the main thread,
    not the workers. */
struct timedWork {
    UA_WorkItem workItem;
    UA_DateTime time;
};

/** Repeated events must ensure that the data is properly deleted at close */
struct repeatedWork {
    struct timedWork next;
    UA_Guid id;
    UA_UInt32 interval; // in 100ns resolution
};

void processWorkItem(UA_Server *server, const UA_WorkItem *item) {
    switch(item->type) {
    case UA_WORKITEMTYPE_BINARYNETWORKMESSAGE:
        UA_Server_processBinaryMessage(server, item->item.binaryNetworkMessage.connection,
                                       &item->item.binaryNetworkMessage.message);
        UA_ByteString_deleteMembers((UA_ByteString *)&item->item.binaryNetworkMessage.message);
        break;
    case UA_WORKITEMTYPE_BINARYNETWORKCLOSED:
        // todo: internal cleanup in the server.
        if(item->item.binaryNetworkMessage.connection->channel)
            // remove the connection from the securechannel.
            item->item.binaryNetworkClose->channel->connection = UA_NULL;
        UA_free(item->item.binaryNetworkClose); // TODO!! Not so fast. Others might still use it.
        break;
    case UA_WORKITEMTYPE_METHODCALL:
        item->item.methodCall.method(server, item->item.methodCall.data);
        break;
    default: {}
        // do nothing
    }
}

#ifdef UA_MULTITHREADING

/** Entry in the dipatch queue */
struct workListNode {
    struct cds_wfcq_node node; // node for the queue
    UA_UInt32 workSize;
    UA_WorkItem *work;
};

// throwaway struct to bring data into the worker threads
struct workerStartData {
    UA_Server *server;
    UA_UInt32 **workerCounter;
};

/** Waits until work arrives in the dispatch queue (restart after 10ms) and processes it. */
void * workerLoop(struct workerStartData *startInfo) {
    UA_Server_registerThread();
    UA_UInt32 *c = UA_alloc(sizeof(UA_UInt32));
    uatomic_set(c, 0);

    *startInfo->workerCounter = c;
    UA_Server *server = startInfo->server;
    UA_free(startInfo);
    
    while(*server->running) {
        struct workListNode *wln = (struct workListNode*)
            cds_wfcq_dequeue_blocking(&server->dispatchQueue_head, &server->dispatchQueue_tail);
        if(wln) {
            for(UA_UInt32 i = 0;i<wln->workSize;i++)
                processWorkItem(server, &wln->work[i]);
            UA_free(wln->work);
            UA_free(wln);
        }
        uatomic_inc(c); // increase the workerCounter;
    }
    UA_Server_unregisterThread();
    return UA_NULL;
}

#endif

/**
 * Delayed Work
 *
 * Sometimes, a method call is allowed only when all work items that were send
 * to dispatch earlier have finished. For example, to remove a UA_Connection, no
 * prior workitem may still posess a pointer to the connection.
 *
 * We collect "delayed work" in a delayedWorkList and regularly check up on
 * them. The process is as follows:
 *
 * 1) Dispatch a workitem with a methodCall to getCounter on the
 * delayedWorkList. Once the method is called we know that the prior workitems
 * are no longer be in dispatch. But they might be in a worker, waiting for
 * execution.
 *
 * 2) The getCounter function collects a counter value from each worker thread.
 * When this counter changes, we know that all current work in the worker thread
 * is done also.
 *
 * 3) We regularly (every 100ms or so) check if a delayedWorkList can be
 * dispatched.
 */
#define DELAYEDWORKSIZE 100
struct delayedWork {
    TAILQ_ENTRY(delayedWorkList) pointers;
    UA_UInt32 *startCounters; // initially UA_NULL until a workitem gets the counters
    UA_UInt32 workItemsCount; // the size of the array is DELAYEDWORKSIZE, the count may be less
    UA_WorkItem *workItems; // when it runs full, a new delayedWork entry is created
};
TAILQ_HEAD(delayedWorkQueue, delayedWork);

// Dispatched as a methodcall
static void getCounter(UA_Server *server, struct delayedWork *delayed) {
    UA_UInt32 *counters = UA_alloc(server->nThreads * sizeof(UA_UInt32));
    for(UA_UInt16 i = 0;i<server->nThreads;i++)
        delayed->startCounters[i] = *server->workerCounters[i];
    delayed->startCounters = counters;
}

static void walkDelayedWorkQueue(UA_Server *server, struct delayedWorkQueue *queue) {
    struct delayedWork *dw;
    TAILQ_FOREACH_REVERSE(dw, queue, delayedWorkQueue, pointers) {
        // 1) Test if ready. If not, the once afterwards are not ready either
        if(!dw->startCounters)
            break;

        for(UA_UInt16 i=0;i<server->nThreads;i++) {
            if(*server->workerCounters[i] == dw->startCounters[i])
                break;
        }

        // 2) Ok. Dispatch the work
        struct workListNode *wln = UA_alloc(sizeof(struct workListNode));
        *wln = (struct workListNode){.workSize = dw->workItemsCount, .work = dw->workItems};
        cds_wfcq_node_init(&wln->node);
        cds_wfcq_enqueue(&server->dispatchQueue_head, &server->dispatchQueue_tail, &wln->node);
    }
}

#ifdef UA_MULTITHREADING

/**
 * # Main server thread
 *
 * ## Handling of timed work
 *
 * ## Handling of delayed work
 * The server always has a delayedWorkObject that is not in the queue. There,
 * delayed work is added until it runs full. Then it gets added to the queue and
 * a methodcall to getCounter is dispatched. Also, a repeatedEvent calls
 * walkDelayedWorkQueue every 100ms.
 */
UA_StatusCode UA_Server_run(UA_Server *server, UA_UInt16 nThreads, UA_Boolean *running) {
    // 1) Prepare the threads
    server->running = running; // the threads need to access the variable
    server->nThreads = nThreads;
    pthread_t *thr = UA_alloca(nThreads * sizeof(pthread_t));
    server->workerCounters = UA_alloc(nThreads * sizeof(UA_UInt32 *));
    for(UA_UInt32 i=0;i<nThreads;i++) {
        struct workerStartData *startData = UA_alloc(sizeof(struct workerStartData));
        startData->server = server;
        startData->workerCounter = &server->workerCounters[i];
        pthread_create(&thr[i], UA_NULL, (void* (*)(void*))workerLoop, startData);
    }

    // 2) Queue-List of delayedWork
    struct delayedWorkQueue delayedWork;
    TAILQ_INIT(&delayedWork);
    struct delayedWork *dw = UA_alloc(sizeof(struct delayedWork));
    dw->workItems = UA_alloc(sizeof(UA_WorkItem) * DELAYEDWORKSIZE);
    dw->startCounters = UA_NULL;
    dw->workItemsCount = 0;
    TAILQ_INSERT_HEAD(&delayedWork, dw, pointers);

    while(*running) {
        // Check if messages have arrived and handle them.
        for(UA_Int32 i=0;i<server->nlsSize;i++) {
            UA_NetworkLayer *nl = &server->nls[i];
            UA_WorkItem *work;
            UA_Int32 workSize = nl->getWork(nl->nlhandle, &work);
            if(workSize<=0) continue;
            struct workListNode *wln = UA_alloc(sizeof(struct workListNode));
            *wln = (struct workListNode){.workSize = workSize, .work = work};
            cds_wfcq_node_init(&wln->node);
            cds_wfcq_enqueue(&server->dispatchQueue_head, &server->dispatchQueue_tail, &wln->node);
        }
    }

    for(UA_UInt32 i=0;i<nThreads;i++) {
        pthread_join(thr[i], UA_NULL);
        UA_free(server->workerCounters[i]);
    }

    UA_free(server->workerCounters);
    return UA_STATUSCODE_GOOD;
}

#else

UA_StatusCode UA_Server_run(UA_Server *server, UA_UInt32 nThreads, UA_Boolean *running) {
    while(*running) {
        for(UA_Int32 i=0;i<server->nlsSize;i++) {
            UA_NetworkLayer *nl = &server->nls[i];
            UA_WorkItem *work;
            UA_Int32 workSize = nl->getWork(nl->nlhandle, &work);
            for(UA_UInt32 i = 0;i<workSize;i++)
                processWorkItem(server, &work[i]);
            UA_free(work);
        }
    }
    return UA_STATUSCODE_GOOD;
}

#endif
