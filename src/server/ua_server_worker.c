#include "ua_server_internal.h"

static void processWorkItems(UA_Server *server, const UA_WorkItem *work, UA_Int32 workSize) {
    for(UA_Int32 i = 0;i<workSize;i++) {
        const UA_WorkItem *item = &work[i];
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
}

#ifdef UA_MULTITHREADING

/** Entry in the dipatch queue */
struct workListNode {
    struct cds_wfcq_node node; // node for the queue
    UA_UInt32 workSize;
    UA_WorkItem *work;
};

static void dispatchWork(UA_Server *server, UA_Int32 workSize, UA_WorkItem *work) {
    if(workSize <= 0)
        return;
    struct workListNode *wln = UA_alloc(sizeof(struct workListNode));
    *wln = (struct workListNode){.workSize = workSize, .work = work};
    cds_wfcq_node_init(&wln->node);
    cds_wfcq_enqueue(&server->dispatchQueue_head, &server->dispatchQueue_tail, &wln->node);
}

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
            processWorkItems(server, wln->work, wln->workSize);
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
 * Timed Work
 *
 * Timed work is stored in a (ordered by dispatch time) linked list of workitem arrays.
 */

/** The timed event and repeated event are only relevant for the main thread,
    not the workers. */
struct UA_TimedWork {
    SLIST_ENTRY(UA_TimedWork) pointers;
    UA_UInt16 workCount; // size is always 100, count may be less
    UA_WorkItem *work;
    UA_Guid *workIds; // same lengths as the work
    UA_DateTime time;
    UA_UInt32 repetitionInterval; // in 100ns resolution, 0 means no repetition
};

void insertTimedWork(UA_Server *server, UA_TimedWork *newTW) {
    UA_TimedWork *tw, *oldTW;
    SLIST_FOREACH(tw, &server->timedWork, pointers) {
        oldTW = tw;
        if(tw->time > newTW->time)
            break;
    };
    SLIST_INSERT_AFTER(oldTW, newTW, pointers);
}

#define BATCHSIZE 20

/* void processTimedWork(UA_Server *server) { */
/*     // When an entry has workCount > 5, dispatch right away. Otherwise, collect */
/*     // them until you have BATCHSIZE items. Dispatch these together and continue */
/*     // until the first time workitem is reached that lies in the future. */

/* #ifdef UA_MULTITHREADING */
/*     UA_WorkItem *items = UA_alloc(20 * sizeof(UA_WorkItem)); */
/* #endif */

/*     UA_DateTime current = UA_DateTime_now(); */
/*     UA_TimedWork *tw = SLIST_FIRST(&server->timedWork); */
/*     while(tw) { */
/*         if(tw->time < current) */
/*             break; */

/* #ifdef UA_MULTITHREADING */
/*         UA_WorkItem *items = UA_alloc(20) */
        
/* #else */
/*         processWorkItems(server,tw->work, tw->workCount); */
/*         UA_free(tw->work); */
/* #endif */
/*         SLIST_REMOVE(&server->timedWork, tw, UA_TimedWork, pointers);  */
/*         if(tw->repetitionInterval > 0) { */
/*             tw->time += tw->repetitionInterval; */
/*         } else { */
/*             UA_free(tw->workIds);  */
/*             UA_free(tw); */
/*         } */
        
/*         tw = SLIST_FIRST(&server->timedWork); */
/*     } */
/* } */

UA_Guid UA_Server_addTimedWorkItem(UA_Server *server, UA_WorkItem **work, UA_DateTime time);
UA_Boolean UA_Server_removeTimedWorkItem(UA_Server *server, UA_Guid workId);

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

struct UA_DelayedWork {
    TAILQ_ENTRY(UA_DelayedWork) pointers;
    UA_UInt32 *startCounters; // initially UA_NULL until a workitem gets the counters
    UA_UInt32 workItemsCount; // the size of the array is DELAYEDWORKSIZE, the count may be less
    UA_WorkItem *workItems; // when it runs full, a new delayedWork entry is created
};

// Dispatched as a methodcall
static void getCounter(UA_Server *server, UA_DelayedWork *delayed) {
    UA_UInt32 *counters = UA_alloc(server->nThreads * sizeof(UA_UInt32));
    for(UA_UInt16 i = 0;i<server->nThreads;i++)
        delayed->startCounters[i] = *server->workerCounters[i];
    delayed->startCounters = counters;
}

static void walkDelayedWorkQueue(UA_Server *server) {
    UA_DelayedWork *dw;
    TAILQ_FOREACH_REVERSE(dw, &server->delayedWork, UA_DelayedWorkQueue, pointers) {
        // 1) Test if ready. If not, the once afterwards are not ready either
        if(!dw->startCounters)
            break;

        for(UA_UInt16 i=0;i<server->nThreads;i++) {
            if(*server->workerCounters[i] == dw->startCounters[i])
                break;
        }

        // 2) Ok. Dispatch the work
        dispatchWork(server, dw->workItemsCount, dw->workItems);

        // 3) Remove the entry
        TAILQ_REMOVE(&server->delayedWork, dw, pointers);
    }
}

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
#ifdef UA_MULTITHREADING
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
#endif

    // 2) Queue-List of delayedWork
    TAILQ_INIT(&server->delayedWork);
    UA_DelayedWork *dw = UA_alloc(sizeof(UA_DelayedWork));
    dw->workItems = UA_alloc(sizeof(UA_WorkItem) * DELAYEDWORKSIZE);
    dw->startCounters = UA_NULL;
    dw->workItemsCount = 0;
    TAILQ_INSERT_HEAD(&server->delayedWork, dw, pointers);

    while(*running) {
        // Check if messages have arrived and handle them.
        for(UA_Int32 i=0;i<server->nlsSize;i++) {
            UA_NetworkLayer *nl = &server->nls[i];
            UA_WorkItem *work;
            UA_Int32 workSize = nl->getWork(nl->nlhandle, &work);
#ifdef UA_MULTITHREADING
            dispatchWork(server, workSize, work);
#else
            processWorkItems(server, work, workSize);
            UA_free(work);
#endif
        }
    }

#ifdef UA_MULTITHREADING
    for(UA_UInt32 i=0;i<nThreads;i++) {
        pthread_join(thr[i], UA_NULL);
        UA_free(server->workerCounters[i]);
    }
    UA_free(server->workerCounters);
#endif

    return UA_STATUSCODE_GOOD;
}
