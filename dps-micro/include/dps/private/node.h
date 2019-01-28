/**
 * @file
 * Local and remote node macros and functions
 */

/*
 *******************************************************************
 *
 * Copyright 2016 Intel Corporation All rights reserved.
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

#ifndef _DPS_NODE_H
#define _DPS_NODE_H

#include <stdint.h>
#include <dps/private/cose.h>
#include <dps/uuid.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DPS_MSG_VERSION 1 /**< DPS protocol version */

/*
 * DPS message types
 */
#define DPS_MSG_TYPE_PUB  1   /**< Publication */
#define DPS_MSG_TYPE_SUB  2   /**< Subscription */
#define DPS_MSG_TYPE_ACK  3   /**< End-to-end publication acknowledgement */
#define DPS_MSG_TYPE_SAK  4   /**< One-hop subscription acknowledgement */

#define DPS_NODE_CREATED      0 /**< Node is created */
#define DPS_NODE_RUNNING      1 /**< Node is running */
#define DPS_NODE_STOPPING     2 /**< Node is stopping */
#define DPS_NODE_STOPPED      3 /**< Node is stopped */

/**
 * Opaque type for platform-specific network state
 */
typedef struct _DPS_Network DPS_Network;
typedef struct _DPS_KeyStore DPS_KeyStore;
typedef struct _DPS_Subscription DPS_Subscription;

#define DPS_TX_HEADER_SIZE       32
#define DPS_TX_BUFFER_SIZE     2048
#define DPS_TMP_BUFFER_SIZE    2048

/**
 * Type for a DPS node
 */
typedef struct _DPS_Node {
    uint8_t tmpBuffer[DPS_TMP_BUFFER_SIZE];
    size_t tmpLen;
    uint8_t txBuffer[DPS_TX_HEADER_SIZE + DPS_TX_BUFFER_SIZE];
    size_t txHdrLen;
    size_t txLen;
    const char* separators;
    uint16_t port;
    COSE_Entity signer;
    DPS_Network* network;
    DPS_KeyStore* keyStore;
    DPS_Subscription* subscriptions;
} DPS_Node;

/**
 * Make a nonce for a specific message type
 *
 * @param uuid The publication UUID
 * @param seqNum The publication sequence number
 * @param msgType The message type (DPS_MSG_TYPE_PUB or DPS_MSG_TYPE_ACK)
 * @param nonce The computed nonce
 */
void DPS_MakeNonce(const DPS_UUID* uuid, uint32_t seqNum, uint8_t msgType, uint8_t nonce[COSE_NONCE_LEN]);

#ifdef __cplusplus
}
#endif

#endif
