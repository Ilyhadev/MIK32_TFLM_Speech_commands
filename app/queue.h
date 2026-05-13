#ifndef QUEUE_H
#define QUEUE_H

#include "app_types.h"

typedef struct Queue* ByteArrayQueue;

ByteArrayQueue Queue_Create(int capacity);
void Queue_Destroy(ByteArrayQueue queue);
bool Queue_IsEmpty(ByteArrayQueue queue);
unsigned int Queue_GetSize(ByteArrayQueue queue);
void Queue_Push(ByteArrayQueue queue, ByteArray *array);
void Queue_PushFromISR(ByteArrayQueue queue, ByteArray *array);
ByteArray Queue_Pop(ByteArrayQueue queue);
ByteArray Queue_PopFromISR(ByteArrayQueue queue);

#endif /* INCLUDE_HELPERS_QUEUE_H_ */
