#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>
#include <assert.h>
#include "testing_networklayers.h"

typedef struct {
	UA_Connection connection;
    const char *filename;
	UA_Int32 filefd;
    void (*writeCallback)(void *, UA_ByteStringArray buf);
    void (*readCallback)(void);
    void *callbackHandle;
} NetworkLayer_FileInput;

/** Accesses only the sockfd in the handle. Can be run from parallel threads. */
static void writeCallback(NetworkLayer_FileInput *handle, UA_ByteStringArray gather_buf) {
    handle->writeCallback(handle->callbackHandle, gather_buf);
}

static void closeCallback(NetworkLayer_FileInput *handle) {
    close(handle->filefd);
}

static UA_StatusCode NetworkLayer_FileInput_start(NetworkLayer_FileInput *layer, UA_Logger *logger) {
    return UA_STATUSCODE_GOOD;
}

static UA_Byte getVal(char c) {
    UA_Byte rtVal = 0;
    if(c >= '0' && c <= '9')
        rtVal = c - '0';
    else
        rtVal = c - 'a' + 10;
    return rtVal;
}

static UA_Int32
NetworkLayer_FileInput_getWork(NetworkLayer_FileInput *layer, UA_WorkItem **workItems, UA_UInt16 timeout)
{
    layer->readCallback();

    // open a new connection
    // return a single buffer with the entire file
    layer->filefd = open(layer->filename, O_RDONLY);
    if(layer->filefd == -1)
        return 0;

    UA_Byte *buf = malloc(layer->connection.localConf.maxMessageSize);
    size_t pos = 0;
    char c,c2;
    UA_Boolean comment = UA_FALSE;
    // it is assumed that hex codes always come in pairs.
    while(read(layer->filefd, &c, 1) > 0) {
        if(c == ' ' || c == '\n')
            continue;
        if(read(layer->filefd, &c2, 1) < 0)
            break;
        if(c == '/' && c2 == '*') {
            comment = UA_TRUE;
            continue;
        } else if(c == '*' && c2 == '/') {
            comment = UA_FALSE;
            continue;
        }
        if(comment)
            continue;
        buf[pos] = getVal(c) * 16 + getVal(c2);
        pos++;
    }
    close(layer->filefd);
    if(pos == 0) {
        free(buf);
        return 0;
    }
    *workItems = malloc(sizeof(UA_WorkItem));
    UA_WorkItem *work = *workItems;
    work->type = UA_WORKITEMTYPE_BINARYNETWORKMESSAGE;
    work->work.binaryNetworkMessage.connection = &layer->connection;
    work->work.binaryNetworkMessage.message = (UA_ByteString){.length = pos, .data = (UA_Byte*)buf};

    return 1;
}

static UA_Int32 NetworkLayer_FileInput_stop(NetworkLayer_FileInput * layer, UA_WorkItem **workItems) {
    // remove the connection in the server
    // return removeAllConnections(layer, workItems);
    return 0;
}

static void NetworkLayer_FileInput_delete(NetworkLayer_FileInput *layer) {
	free(layer);
}


UA_ServerNetworkLayer
ServerNetworkLayerFileInput_new(const char *filename, void(*readCallback)(void),
                                void(*writeCallback) (void*, UA_ByteStringArray buf),
                                void *callbackHandle)
{
    NetworkLayer_FileInput *layer = malloc(sizeof(NetworkLayer_FileInput));
    layer->connection.state = UA_CONNECTION_OPENING;
    layer->connection.localConf = UA_ConnectionConfig_standard;
    layer->connection.channel = (void*)0;
    layer->connection.close = (void (*)(void*))closeCallback;
    layer->connection.write = (void (*)(void*, UA_ByteStringArray))writeCallback;

    layer->filename = filename;
    layer->filefd = -1;
    layer->readCallback = readCallback;
    layer->writeCallback = writeCallback;
    layer->callbackHandle = callbackHandle;
    
    UA_ServerNetworkLayer nl;
    nl.nlHandle = layer;
    nl.start = (UA_StatusCode (*)(void*, UA_Logger *logger))NetworkLayer_FileInput_start;
    nl.getWork = (UA_Int32 (*)(void*, UA_WorkItem**, UA_UInt16)) NetworkLayer_FileInput_getWork;
    nl.stop = (UA_Int32 (*)(void*, UA_WorkItem**)) NetworkLayer_FileInput_stop;
    nl.free = (void (*)(void*))NetworkLayer_FileInput_delete;
    return nl;
}
