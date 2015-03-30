#ifndef TESTING_NETWORKLAYERS_H_
#define TESTING_NETWORKLAYERS_H_

#include "ua_server.h"

/** @brief Create the TCP networklayer and listen to the specified port */
UA_ServerNetworkLayer ServerNetworkLayerFileInput_new(const char *filename,
                                                      void(*readCallback)(void),
                                                      void(*writeCallback) (void*, UA_ByteStringArray buf),
                                                      void *callbackHandle);

#endif /* TESTING_NETWORKLAYERS_H_ */
