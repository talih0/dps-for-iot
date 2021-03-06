/*
 *******************************************************************
 *
 * Copyright 2018 Intel Corporation All rights reserved.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
 */
#include "test.h"
#include "keys.h"
#include <ctype.h>
#include <dps/dps.h>
#include <dps/targets.h>

static char testString[] = "This is a test string from " DPS_TARGET_NAME;

#define NUM_TOPICS 2

static const char* topics[NUM_TOPICS] = {
    "red/green/blue",
    "a/b/c/d"
};

static void OnPub(DPS_Subscription* sub, const DPS_Publication* pub, uint8_t* payload, size_t len)
{
    int txtLen = 0;
    DPS_PRINT("Received matching publication %d bytes\n", len);

    while (txtLen <= len && isprint(payload[txtLen])) {
        ++txtLen;
    }
    DPS_PRINT("%.*s\n", txtLen, payload);

}

static void PubSendComplete(DPS_Publication* pub, const uint8_t* data, DPS_Status status)
{
    DPS_PRINT("PubSendComplete %s\n", DPS_ErrTxt(status));
}

int main(int argc, char** argv)
{
    DPS_Node* node;
    DPS_KeyStore* keyStore = NULL;
    DPS_Publication* pub;
    DPS_Subscription* sub;
    DPS_Status status;
    int dtls = DPS_TRUE;
    int i;
    int numPubs;
    char* host = NULL;
    int port = 0;
    const DPS_NodeAddress* destAddr = NULL;

#if DPS_TARGET == DPS_TARGET_WINDOWS || DPS_TARGET == DPS_TARGET_LINUX
    char** arg = argv + 1;

    DPS_Debug = DPS_FALSE;
    while (--argc) {
        if (strcmp(*arg, "-d") == 0) {
            ++arg;
            DPS_Debug = DPS_TRUE;
            continue;
        }
        if (strcmp(*arg, "-s") == 0) {
            ++arg;
            dtls = DPS_FALSE;
            continue;
        }
        if (strcmp(*arg, "-h") == 0) {
            ++arg;
            if (--argc == 0) {
                goto Usage;
            }
            host = *arg++;
            continue;
        }
        if (strcmp(*arg, "-p") == 0) {
            ++arg;
            if (--argc == 0) {
                goto Usage;
            }
            sscanf(*arg++, "%d", &port);
            continue;
        }
        goto Usage;
    }
    numPubs = 10;
#else
    DPS_Debug = DPS_TRUE;
    numPubs = INT_MAX;
#endif

    if (port) {
        destAddr = DPS_InitNodeAddress(host, (uint16_t)port);
        if (!destAddr) {
            goto Usage;
        }
        DPS_PRINT("Will publish to destination node %s:%d\n", DPS_NodeAddrToString(destAddr), (uint16_t)port);
    }

    DPS_PRINT("Starting pub unit test\n");

    node = DPS_CreateNode("/");

    /* For testing purposes manually add keys to the key store */
    keyStore = DPS_GetKeyStore(node);
    for (i = 0; i < NUM_KEYS; ++i) {
        status = DPS_SetContentKey(keyStore, &PskId[i], &Psk[i]);
        CHECK(status == DPS_OK);
    }

    /* Network key for DTLS */
    status = DPS_SetNetworkKey(keyStore, &NetworkKeyId, &NetworkKey);
    CHECK(status == DPS_OK);

    status = DPS_SetTrustedCA(keyStore, TrustedCAs);
    CHECK(status == DPS_OK);
    status = DPS_SetCertificate(keyStore, Ids[PUB_ID].cert, Ids[PUB_ID].privateKey, Ids[PUB_ID].password);
    CHECK(status == DPS_OK);

    status = DPS_Start(node);
    CHECK(status == DPS_OK);

    if (!dtls) {
        status = DPS_DisableDTLS(node);
        CHECK(status == DPS_OK);
        DPS_PRINT("DTLS is disabled\n");
    }

    /* Initialize publication with a pre-shared key */
    pub = DPS_InitPublication(node, topics, NUM_TOPICS, DPS_FALSE, &PskId[1], NULL);
    CHECK(pub != NULL);

    sub = DPS_InitSubscription(node, topics, 1);
    CHECK(sub != NULL);

    status = DPS_Subscribe(sub, OnPub, NULL);
    CHECK(status == DPS_OK);

    for (i = 0; i < numPubs; ++i) {
        status = DPS_Publish(pub, destAddr, (const uint8_t*)testString, strlen(testString) + 1, 0, PubSendComplete);
        CHECK(status == DPS_OK);
        SLEEP(5000);
    }

    return 0;

    DPS_DestroySubscription(sub);
    DPS_DestroyPublication(pub);

failed:
    DPS_PRINT("FAILED: status=%s (%s) near line %d\r\n", DPS_ErrTxt(status), __FILE__, atLine - 1);
#if DPS_TARGET == DPS_TARGET_ZEPHYR
    while (1) {
        SLEEP(100);
    }
#else
    return 1;
Usage:
    DPS_PRINT("Usage %s: [-d] [-s] [-h <host-addr>] [-p <port>]\n", argv[0]);
#endif
    return 1;
}
