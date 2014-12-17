 /*
 * This work is licensed under a Creative Commons CCZero 1.0 Universal License.
 * See http://creativecommons.org/publicdomain/zero/1.0/ for more information.
 */

#ifdef _WIN32
#include <malloc.h>
#include <winsock2.h>
#include <sys/types.h>
#include <Windows.h>
#include <ws2tcpip.h>
#define CLOSESOCKET(S) closesocket(S)
#else
#include <sys/select.h> 
#include <netinet/in.h>
#include <unistd.h> // read, write, close
#define CLOSESOCKET(S) close(S)
#endif

#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h> // errno, EINTR
#include <fcntl.h> // fcntl

#include "networklayer_tcp.h" // UA_MULTITHREADING is defined in here

#ifdef UA_MULTITHREADING
#include <urcu/uatomic.h>
#endif

#define MAXBACKLOG 100

struct Networklayer_TCP;

/* Forwarded to the server as a (UA_Connection) and used for callbacks back into
   the networklayer */
typedef struct {
	UA_Connection connection;
	UA_Int32 sockfd;
	struct NetworkLayerTCP *layer;
} TCPConnection;

/* Internal mapping of sockets to connections */
typedef struct {
    TCPConnection *connection;
    UA_Int32 sockfd;
} ConnectionLink;

typedef struct NetworkLayerTCP {
	UA_ConnectionConfig conf;
	fd_set fdset;
	UA_Int32 serversockfd;
	UA_Int32 highestfd;
    UA_UInt16 conLinksSize;
    ConnectionLink *conLinks;
    UA_UInt32 port;
#ifdef UA_MULTITHREADING
    // we remove the connection links only in the main thread. Attach
    // to-be-deleted links with atomic operations
    struct deleteLink {
        UA_Int32 sockfd;
        struct deleteLink *next;
    } *deleteLinkList;
#endif
} NetworkLayerTCP;

// the callbacks are thread-safe if UA_MULTITHREADING is defined
void closeCallback(TCPConnection *handle);
void writeCallback(TCPConnection *handle, UA_ByteStringArray gather_buf);

static UA_StatusCode NetworkLayerTCP_add(NetworkLayerTCP *layer, UA_Int32 newsockfd) {
    TCPConnection *c = malloc(sizeof(TCPConnection));
	c->sockfd = newsockfd;
    c->layer = layer;
    c->connection.state = UA_CONNECTION_OPENING;
    c->connection.localConf = layer->conf;
    c->connection.channel = UA_NULL;
    c->connection.close = (void (*)(void*))closeCallback;
    c->connection.write = (void (*)(void*, UA_ByteStringArray))writeCallback;

    layer->conLinks = realloc(layer->conLinks, sizeof(ConnectionLink)*(layer->conLinksSize+1));
    layer->conLinks[layer->conLinksSize].connection = c;
    layer->conLinks[layer->conLinksSize].sockfd = newsockfd;
    layer->conLinksSize++;
	return UA_STATUSCODE_GOOD;
}

#ifdef UA_MULTITHREADING

void closeCallback(TCPConnection *handle) {
    if(uatomic_xchg(&handle->connection.state, UA_CONNECTION_CLOSING) == UA_CONNECTION_CLOSING)
        return;

    UA_Connection_detachSecureChannel(&handle->connection);
    // Remove the link later in the main thread
    struct deleteLink *d = malloc(sizeof(struct deleteLink));
    d->sockfd = handle->sockfd;
    while(1) {
        d->next = handle->layer->deleteLinkList;
        if(uatomic_cmpxchg(&handle->layer->deleteLinkList, d->next, d) == (void*)&d->next)
            break;
    }
}

// Not thread-safe (call only from main loop). Returns a pointer to the
// connection (for delayed delete)
static TCPConnection * NetworkLayerTCP_remove(NetworkLayerTCP *layer, UA_Int32 sockfd) {
	UA_Int32 i;
	for(i = 0;i < layer->conLinksSize;i++) {
		if(layer->conLinks[i].sockfd == sockfd)
			break;
	}
    if(i >= layer->conLinksSize)
        return UA_NULL;

    // maybe this is done twice. doesn't hurt since the pointers are checked for
    // detaching and the shutdown is clean.
    UA_Connection_detachSecureChannel(&layer->conLinks[i].connection->connection);
	shutdown(sockfd,2);
	CLOSESOCKET(sockfd);

    TCPConnection *c = layer->conLinks[i].connection;
    layer->conLinksSize--;
    layer->conLinks[i] = layer->conLinks[layer->conLinksSize];
    return c;
}

// Returns work for delayed execution
static UA_UInt32 removeLinks(NetworkLayerTCP *layer, UA_WorkItem **returnWork) {
    UA_WorkItem *work = malloc(sizeof(UA_WorkItem)*layer->conLinksSize);
    UA_UInt32 count = 0;
    struct deleteLink *d = uatomic_xchg(&layer->deleteLinkList, UA_NULL);
    while(d) {
        work[count] = (UA_WorkItem){.type = UA_WORKITEMTYPE_DELAYEDFREE,
                                    .item.delayedFree = NetworkLayerTCP_remove(layer,d->sockfd)};
        struct deleteLink *oldd = d;
        d = d->next;
        free(oldd);
        count++;
    }
    *returnWork = work;
    return count;
}

#else

static void NetworkLayerTCP_remove(NetworkLayerTCP *layer, UA_Int32 sockfd) {
	UA_Int32 i;
	for(i = 0;i < layer->conLinksSize;i++) {
		if(layer->conLinks[i].sockfd == sockfd)
			break;
	}
    if(i >= layer->conLinksSize)
        return;

    UA_Connection_detachSecureChannel(&layer->conLinks[i].connection->connection);
	shutdown(sockfd,2);
	CLOSESOCKET(sockfd);
    free(layer->conLinks[i].connection);

    // copy the last element over the removed entry
    layer->conLinksSize--;
    layer->conLinks[i] = layer->conLinks[layer->conLinksSize];
}

// ensure that the connection is detached from the securechannel before
void closeCallback(TCPConnection *handle) {
    if(handle->connection.state == UA_CONNECTION_CLOSING)
        return;
    handle->connection.state = UA_CONNECTION_CLOSING;
    NetworkLayerTCP_remove(handle->layer, handle->sockfd);
}

#endif

/** Accesses only the sockfd in the handle. Can be run from parallel threads. */
void writeCallback(TCPConnection *handle, UA_ByteStringArray gather_buf) {
	UA_UInt32 total_len = 0, nWritten = 0;
#ifdef _WIN32
	LPWSABUF buf = _alloca(gather_buf.stringsSize * sizeof(WSABUF));
	int result = 0;
	for(UA_UInt32 i = 0; i<gather_buf.stringsSize; i++) {
		buf[i].buf = gather_buf.strings[i].data;
		buf[i].len = gather_buf.strings[i].length;
		total_len += gather_buf.strings[i].length;
	}
	while(nWritten < total_len) {
		UA_UInt32 n=0;
		do {
			result = WSASend(handle->sockfd, buf, gather_buf.stringsSize ,
                             (LPDWORD)&n, 0, NULL, NULL);
			if(result != 0)
				printf("Error WSASend, code: %d \n", WSAGetLastError());
		} while(errno == EINTR);
		nWritten += n;
	}
#else
	struct iovec iov[gather_buf.stringsSize];
	for(UA_UInt32 i=0;i<gather_buf.stringsSize;i++) {
		iov[i] = (struct iovec) {.iov_base = gather_buf.strings[i].data,
                                 .iov_len = gather_buf.strings[i].length};
		total_len += gather_buf.strings[i].length;
	}
	struct msghdr message = {.msg_name = NULL, .msg_namelen = 0, .msg_iov = iov,
							 .msg_iovlen = gather_buf.stringsSize, .msg_control = NULL,
							 .msg_controllen = 0, .msg_flags = 0};
	while (nWritten < total_len) {
		UA_Int32 n = 0;
		do {
            n = sendmsg(handle->sockfd, &message, 0);
        } while (n == -1L && errno == EINTR);

		if(n >= 0) {
			// TODO: handle incompletely send messages
			/* nWritten += n; */
			break;
		} else {
			// TODO: error handling
			break;
		}
	}
#endif
}

static UA_StatusCode setNonBlocking(int sockid) {
#ifdef _WIN32
	u_long iMode = 1;
	if(ioctlsocket(sockid, FIONBIO, &iMode) != NO_ERROR)
		return UA_STATUSCODE_BADINTERNALERROR;
#else
	int opts = fcntl(sockid,F_GETFL);
	if(opts < 0 || fcntl(sockid,F_SETFL,opts|O_NONBLOCK) < 0)
		return UA_STATUSCODE_BADINTERNALERROR;
#endif
	return UA_STATUSCODE_GOOD;
}

// after every select, reset the set of sockets we want to listen on
static void setFDSet(NetworkLayerTCP *layer) {
	FD_ZERO(&layer->fdset);
	FD_SET(layer->serversockfd, &layer->fdset);
	layer->highestfd = layer->serversockfd;
	for(UA_Int32 i=0;i<layer->conLinksSize;i++) {
		FD_SET(layer->conLinks[i].sockfd, &layer->fdset);
		if(layer->conLinks[i].sockfd > layer->highestfd)
			layer->highestfd = layer->conLinks[i].sockfd;
	}
}

UA_Int32 NetworkLayerTCP_getWork(NetworkLayerTCP * layer, UA_WorkItem **workItems, UA_UInt16);
UA_Int32 NetworkLayerTCP_stop(NetworkLayerTCP * layer, UA_WorkItem **workItems);
void NetworkLayerTCP_delete(NetworkLayerTCP *layer);
UA_StatusCode NetworkLayerTCP_start(NetworkLayerTCP *layer);

UA_NetworkLayer * NetworkLayerTCP_new(UA_ConnectionConfig conf, UA_UInt32 port) {
    NetworkLayerTCP *tcplayer = malloc(sizeof(NetworkLayerTCP));
    if(!tcplayer)
        return NULL;
	tcplayer->conf = conf;
	tcplayer->conLinksSize = 0;
	tcplayer->conLinks = NULL;
    tcplayer->port = port;
#ifdef UA_MULTITHREADING
    tcplayer->deleteLinkList = UA_NULL;
#endif

    UA_NetworkLayer *nl = malloc(sizeof(UA_NetworkLayer));
    if(!nl) {
        free(tcplayer);
        return NULL;
    }
    nl->nlHandle = tcplayer;
    nl->start = (UA_StatusCode (*)(void*))NetworkLayerTCP_start;
    nl->getWork = (UA_Int32 (*)(void*, UA_WorkItem**, UA_UInt16)) NetworkLayerTCP_getWork;
    nl->stop = (UA_Int32 (*)(void*, UA_WorkItem**)) NetworkLayerTCP_stop;
    nl->delete = (void (*)(void*))NetworkLayerTCP_delete;
    return nl;
}

UA_StatusCode NetworkLayerTCP_start(NetworkLayerTCP *layer) {
#ifdef _WIN32
	WORD wVersionRequested;
	WSADATA wsaData;
	wVersionRequested = MAKEWORD(2, 2);
	WSAStartup(wVersionRequested, &wsaData);
	if((layer->serversockfd = socket(PF_INET, SOCK_STREAM,0)) == INVALID_SOCKET) {
		printf("ERROR opening socket, code: %d\n", WSAGetLastError());
		return UA_STATUSCODE_BADINTERNALERROR;
	}
#else
    if((layer->serversockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		perror("ERROR opening socket");
		return UA_STATUSCODE_BADINTERNALERROR;
	} 
#endif

	struct sockaddr_in serv_addr;
	memset((void *)&serv_addr, sizeof(serv_addr), 1);
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = INADDR_ANY;
	serv_addr.sin_port = htons(layer->port);

	int optval = 1;
	if(setsockopt(layer->serversockfd, SOL_SOCKET,
                  SO_REUSEADDR, (const char *)&optval,
                  sizeof(optval)) == -1) {
		perror("setsockopt");
		CLOSESOCKET(layer->serversockfd);
		return UA_STATUSCODE_BADINTERNALERROR;
	}
		
	if(bind(layer->serversockfd, (struct sockaddr *) &serv_addr,
            sizeof(serv_addr)) < 0) {
		perror("binding");
		CLOSESOCKET(layer->serversockfd);
		return UA_STATUSCODE_BADINTERNALERROR;
	}

	setNonBlocking(layer->serversockfd);
	listen(layer->serversockfd, MAXBACKLOG);
    printf("Listening for TCP connections on %s:%d\n",
           inet_ntoa(serv_addr.sin_addr),
           ntohs(serv_addr.sin_port));
    return UA_STATUSCODE_GOOD;
}

UA_Int32 NetworkLayerTCP_getWork(NetworkLayerTCP * layer, UA_WorkItem **workItems,
                                 UA_UInt16 timeout) {
    struct timeval tmptv = {0, timeout};
    UA_WorkItem *items = UA_NULL;
    UA_Int32 itemsCount = 0;
#ifdef UA_MULTITHREADING
    itemsCount = removeLinks(layer, &items);
#endif
    setFDSet(layer);
    UA_Int32 resultsize = select(layer->highestfd+1, &layer->fdset,
                                 NULL, NULL, &tmptv);
    if(resultsize < 0) {
        *workItems = items;
        return itemsCount;
    }

	// accept new connections (can only be a single one)
	if(FD_ISSET(layer->serversockfd,&layer->fdset)) {
		struct sockaddr_in cli_addr;
		socklen_t cli_len = sizeof(cli_addr);
		int newsockfd = accept(layer->serversockfd, (struct sockaddr *) &cli_addr, &cli_len);
		if (newsockfd >= 0) {
			setNonBlocking(newsockfd);
			NetworkLayerTCP_add(layer, newsockfd);
		}
		resultsize--;
	}
    
    if(resultsize <= 0)
        resultsize = 0;
    else
        items = realloc(items, sizeof(UA_WorkItem)*(itemsCount+resultsize));

	// read from established sockets
    UA_Int32 j = itemsCount;
    UA_ByteString buf = {-1, NULL};
	for(UA_Int32 i=0;i<layer->conLinksSize && j<itemsCount+resultsize;i++) {
		if(FD_ISSET(layer->conLinks[i].sockfd, &layer->fdset)) {
            if(buf.data == NULL)
                buf.data = malloc(layer->conf.recvBufferSize);
#ifdef _WIN32
            buf.length = recv(layer->conLinks[i].connection.sockfd, (char *)buf.data,
                              layer->conf.recvBufferSize, 0);
#else
            buf.length = read(layer->conLinks[i].sockfd, buf.data,
                              layer->conf.recvBufferSize);
#endif
            if (errno != 0) {
                items[j].type = UA_WORKITEMTYPE_BINARYNETWORKCLOSED; // the connection (link) gets removed in the callback
                items[j].item.binaryNetworkClosed = &layer->conLinks[i].connection->connection;
                j++;
                continue;
            }

            items[j].type = UA_WORKITEMTYPE_BINARYNETWORKMESSAGE;
            items[j].item.binaryNetworkMessage.message = buf;
            items[j].item.binaryNetworkMessage.connection = &layer->conLinks[i].connection->connection;
            j++;
            buf.data = NULL;
        }
    }

    if(buf.data != NULL)
        free(buf.data);

    *workItems = items;
    return j;
}

UA_Int32 NetworkLayerTCP_stop(NetworkLayerTCP * layer, UA_WorkItem **workItems) {
	for(UA_Int32 index = 0;index < layer->conLinksSize;index++)
        NetworkLayerTCP_remove(layer, layer->conLinks[index].sockfd);
	free(layer->conLinks);
    layer->conLinksSize = 0;
#ifdef _WIN32
	WSACleanup();
#endif

#ifdef UA_MULTITHREADING
    return removeLinks(layer, workItems);
#else
    *workItems = UA_NULL;
    return 0;
#endif
}

void NetworkLayerTCP_delete(NetworkLayerTCP *layer) {
	free(layer);
}
