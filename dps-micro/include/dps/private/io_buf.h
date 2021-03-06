/**
 * @file
 * Internal APIs
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

#ifndef _DPS_IO_BUF_H
#define _DPS_IO_BUF_H

#include <stdint.h>
#include <dps/err.h>
#include <dps/private/dps.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * For managing data that has been received
 */
typedef struct _DPS_RxBuffer {
    uint8_t* base;   /**< base address for buffer */
    uint8_t* eod;    /**< end of data */
    uint8_t* rxPos;  /**< current read location in buffer */
} DPS_RxBuffer;

/**
 * Initialize a receive buffer
 *
 * @param buffer    Buffer to initialized
 * @param storage   The storage for the buffer. The storage cannot be NULL
 * @param size      The size of the storage
 *
 * @return   DPS_OK or DP_ERR_RESOURCES if storage is needed and could not be allocated.
 */
void DPS_RxBufferInit(DPS_RxBuffer* buffer, uint8_t* storage, size_t size);

/**
 * Free resources allocated for a buffer and nul out the buffer pointers.
 *
 * @param buffer    Buffer to free
 */
void DPS_RxBufferFree(DPS_RxBuffer* buffer);

/**
 * Clear receive buffer fields
 */
#define DPS_RxBufferClear(b) do { (b)->base = (b)->rxPos = (b)->eod = NULL; } while (0)

/**
 * Data available in a receive buffer
 */
#define DPS_RxBufferAvail(b)  ((uint32_t)((b)->eod - (b)->rxPos))

typedef enum {
    DPS_TX_POOL,
    DPS_TX_HDR_POOL,
    DPS_TMP_POOL
} DPS_BUFFER_POOL;

/**
 * For managing data to be transmitted
 */
typedef struct _DPS_TxBuffer {
    uint8_t* base;  /**< base address for buffer */
    uint8_t* eob;   /**< end of buffer */
    uint8_t* txPos; /**< current write location in buffer */
    DPS_Node* node;
    DPS_BUFFER_POOL pool;
} DPS_TxBuffer;

/**
 * Add data to a transmit buffer
 *
 * @param buffer   Buffer to append to
 * @param data     The data to append
 * @param len      Length of the data to append
 *
 * @return   DPS_OK or DP_ERR_RESOURCES if there not enough room in the buffer
 */
DPS_Status DPS_TxBufferAppend(DPS_TxBuffer* buffer, const uint8_t* data, size_t len);

/**
 * Clear transmit buffer fields
 */
#define DPS_TxBufferClear(b) do { (b)->base = (b)->txPos = (b)->eob = NULL; } while (0)

/**
 * Space left in a transmit buffer
 */
#define DPS_TxBufferSpace(b)  ((uint32_t)((b)->eob - (b)->txPos))

/**
 * Number of bytes that have been written to a transmit buffer
 */
#define DPS_TxBufferUsed(b)  ((uint32_t)((b)->txPos - (b)->base))

/**
 * Size of transmit buffer
 */
#define DPS_TxBufferCapacity(b)  ((uint32_t)((b)->eob - (b)->base))

/**
 * Convert a transmit buffer into a receive buffer. Note that this
 * aliases the internal storage so care must be taken to avoid a
 * double free.
 *
 * @param txBuffer   A buffer containing data
 * @param rxBuffer   Receive buffer struct to be initialized
 */
void DPS_TxBufferToRx(const DPS_TxBuffer* txBuffer, DPS_RxBuffer* rxBuffer);

/**
 * Convert a receive buffer into a transmit buffer. Note that this
 * aliases the internal storage so care must be taken to avoid a
 * double free.
 *
 * @param rxBuffer   A buffer containing data
 * @param txBuffer   Transmit buffer struct to be initialized
 */
void DPS_RxBufferToTx(const DPS_RxBuffer* rxBuffer, DPS_TxBuffer* txBuffer);


/**
 * Reserves space in one of a node's contiguous buffers. DPS_TxBufferCommit() must be called to finalize allocation of the space,
 *
 * @param node            Pointer to the DPS node
 * @param txBuf           A transmit buffer to return the reserved bytes
 * @param len             Number of bytes to reserve
 * @param pool            Which pool to allocate from
 *
 * @return DPS_OK if the space was reserved
 *         DPS_ERR_RESOURCES if there not enough free space in the transmit buffer
 */
DPS_Status DPS_TxBufferReserve(DPS_Node* node, DPS_TxBuffer* txBuf, size_t len, DPS_BUFFER_POOL pool);

/**
 * Finalizes allocation of space for a Tx buffer by adjusting offsets in the node's contiguous buffer.
 *
 * @param txBuf           A transmit buffer that has been written to
 */
void DPS_TxBufferCommit(DPS_TxBuffer* txBuf);

/**
 * Frees all Tx buffer space
 *
 * @param node            Pointer to the DPS node
 * @param pool            The pool to free
 */
void DPS_TxBufferFreePool(DPS_Node* node, DPS_BUFFER_POOL pool);


#ifdef __cplusplus
}
#endif

#endif
