#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include "app_types.h"

typedef struct CircularBuffer* ByteCircularBuffer;

ByteCircularBuffer ByteCircularBuffer_Create(uint16_t capacity);
void ByteCircularBuffer_Destroy(ByteCircularBuffer circularBuffer);
uint16_t ByteCircularBuffer_GetSize(ByteCircularBuffer circularBuffer);
uint8_t ByteCircularBuffer_IsEmpty(ByteCircularBuffer circularBuffer);
void ByteCircularBuffer_Push(ByteCircularBuffer circularBuffer, uint16_t value);
void ByteCircularBuffer_PushFromISR(ByteCircularBuffer circularBuffer, uint16_t value);
uint16_t ByteCircularBuffer_Pop(ByteCircularBuffer circularBuffer);
uint16_t ByteCircularBuffer_PopFromISR(ByteCircularBuffer circularBuffer);

#endif 
