#include "mik32_hal_irq.h"
#include <stdlib.h>
#include <string.h>
#include "queue.h"

#define CAPACITY 10

struct Queue {
	ByteArray* elements;
	unsigned int size;
	unsigned int front;
	unsigned int back;
	unsigned int capacity;
};

static struct Queue queue;
static ByteArray elements_array[CAPACITY];

static void push(ByteArrayQueue queue, ByteArray *array);
static ByteArray pop(ByteArrayQueue queue);
static void increaseSize(ByteArrayQueue q);
static void decreaseSize(ByteArrayQueue q);
static void resetBackPointerIfReachedBound(ByteArrayQueue q);
static void resetFrontPointerIfReachedBound(ByteArrayQueue q);
static void putBackByteArray(ByteArrayQueue q, ByteArray *a);
static ByteArray getFrontByteArray(ByteArrayQueue q);
static ByteArray getZeroLengthByteArray(void);

struct Queue* Queue_Create(int capacity)
{
	(void)capacity;
	//ByteArrayQueue q = (ByteArrayQueue)malloc(sizeof(struct Queue));
	ByteArrayQueue q = &queue;

	//q->elements = (ByteArray *)malloc(capacity * sizeof(ByteArray));
	q->elements = elements_array;
	q->size  = 0;
	q->front = 0;
	q->back	 = 0;
	q->capacity = CAPACITY;
	
	return q;
}

void Queue_Destroy(struct Queue *queue)
{
	if(0 != queue) 
	{
	//	free(queue->elements);
	//	free(queue);
	}
}

bool Queue_IsEmpty(ByteArrayQueue queue)
{
	bool result;
	
	if(0 == queue->size)
	{
		result = true;
	}
	else
	{
		result = false;		
	}
	
	return result;
}

uint8_t Queue_IsFull(ByteArrayQueue queue)
{
	uint8_t result;
	
	if(queue->capacity == queue->size) 
	{
		result = true;
	} 
	else 
	{
		result = false;
	}
	
	return result;
}

unsigned int Queue_GetSize(ByteArrayQueue queue)
{
	return queue->size;
}

void Queue_Push(ByteArrayQueue queue, ByteArray *array)
{
	HAL_IRQ_DisableInterrupts();
	push(queue, array);
	HAL_IRQ_EnableInterrupts();
}


void Queue_PushFromISR(ByteArrayQueue queue, ByteArray *array)
{
	push(queue, array);
}

ByteArray Queue_Pop(ByteArrayQueue queue)
{
	ByteArray result;
	
	HAL_IRQ_DisableInterrupts();
    result = pop(queue);
	HAL_IRQ_EnableInterrupts();
	
	return result;
}

ByteArray Queue_PopFromISR(ByteArrayQueue queue)
{
	ByteArray result;
		
	result = pop(queue);
	
	return result;
}

static void push(ByteArrayQueue queue, ByteArray *array)
{
	if(Queue_IsFull(queue)) 
	{
		return;
	}
	
	if(0 == array->length)
	{
		return;
	}
	
	putBackByteArray(queue, array);
	increaseSize(queue);
}

static ByteArray pop(ByteArrayQueue queue)
{
	ByteArray result;
	
	if(Queue_IsEmpty(queue)) 
	{
		result = getZeroLengthByteArray();
	}
	else 
	{
		result = getFrontByteArray(queue);
		decreaseSize(queue);
	}
	
	return result;
}

static void increaseSize(ByteArrayQueue q)
{
	q->size++;	
}

static void decreaseSize(ByteArrayQueue q)
{
	q->size--;	
}

static void resetBackPointerIfReachedBound(ByteArrayQueue q)
{
	if(q->capacity == q->back) {
		q->back = 0;
	}
}

static void resetFrontPointerIfReachedBound(ByteArrayQueue q)
{
	if(q->capacity == q->front) {
		q->front = 0;
	}
}

static void putBackByteArray(ByteArrayQueue q, ByteArray *a)
{
	resetBackPointerIfReachedBound(q);

	ByteArray *next = &q->elements[q->back++];

	for(uint8_t i = 0; i < a->length + 1; ++i) {
		*(next + i) = *(a + i);
	}
}

static ByteArray getFrontByteArray(ByteArrayQueue q)
{
	resetFrontPointerIfReachedBound(q);
	return q->elements[q->front++];
}

static ByteArray getZeroLengthByteArray(void)
{
	ByteArray ba;
	ba.length = 0;
	return ba;
}