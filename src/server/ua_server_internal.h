#ifndef UA_SERVER_INTERNAL_H_
#define UA_SERVER_INTERNAL_H_

#include "ua_config.h"

#ifdef MULTITHREADING
#define _LGPL_SOURCE
#include <urcu.h>
#include <urcu/wfqueue.h> // todo: replace with wfcqueue once the lib updates
#endif

#include "ua_server.h"
#include "ua_session_manager.h"
#include "ua_securechannel_manager.h"
#include "ua_nodestore.h"

/** Mapping of namespace-id and url to an external nodestore. For namespaces
    that have no mapping defined, the internal nodestore is used by default. */
typedef struct UA_ExternalNamespace {
	UA_UInt16 index;
	UA_String url;
	UA_ExternalNodeStore externalNodeStore;
} UA_ExternalNamespace;

struct UA_Server {
    UA_ApplicationDescription description;
    UA_Int32 endpointDescriptionsSize;
    UA_EndpointDescription *endpointDescriptions;

    UA_ByteString serverCertificate;
    UA_SecureChannelManager secureChannelManager;
    UA_SessionManager sessionManager;
    UA_Logger logger;

    UA_NodeStore *nodestore;
    UA_Int32 externalNamespacesSize;
    UA_ExternalNamespace *externalNamespaces;

    UA_Int32 nlsSize;
    UA_NetworkLayer *nls;

    #ifdef MULTITHREADING
    UA_Boolean *running;
    // worker threads wait on the queue
    struct cds_wfq_queue dispatchQueue;
    #endif
};

void UA_Server_processBinaryMessage(UA_Server *server, UA_Connection *connection, const UA_ByteString *msg);

UA_AddNodesResult UA_Server_addNodeWithSession(UA_Server *server, UA_Session *session, const UA_Node **node,
                                               const UA_ExpandedNodeId *parentNodeId, const UA_NodeId *referenceTypeId);

UA_StatusCode UA_Server_addReferenceWithSession(UA_Server *server, UA_Session *session, const UA_AddReferencesItem *item);

/** Used by worker threads or the main loop (without concurrency) */
void processWork(UA_Server *server, const UA_WorkItem *items, UA_UInt32 itemsSize);

#ifdef MULTITHREADING
void * runWorkerLoop(UA_Server *server);

struct workListNode {
    struct cds_wfq_node node; // node for the queue
    const UA_UInt32 workSize;
    const UA_WorkItem *work;
};
#endif

#endif /* UA_SERVER_INTERNAL_H_ */
