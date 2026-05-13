#include <stdlib.h>
#include "circular_buffer.h"
#include "mik32_hal_irq.h"

#define zeroValue 0
#define CAPACITY 480

struct CircularBuffer
{
		uint16_t *values;
		uint16_t capacity;
		uint16_t size;
		uint8_t head;
		uint8_t tail;
};

static struct CircularBuffer circular_buffer;
static uint16_t values_array[CAPACITY];

static uint16_t pop(ByteCircularBuffer buffer);
static void push(ByteCircularBuffer buffer, uint16_t value);
static void increaseSize(ByteCircularBuffer buffer);
static void decreaseSize(ByteCircularBuffer buffer);
static void resetHeadIndexIfReachedBound(ByteCircularBuffer buffer);
static void putValue(ByteCircularBuffer buffer, uint16_t value);
static uint16_t getValue(ByteCircularBuffer buffer);
static void resetTailIndexIfReachedBound(ByteCircularBuffer buffer);

ByteCircularBuffer ByteCircularBuffer_Create(uint16_t capacity)
{
	(void)capacity;
	//ByteCircularBuffer buffer = (ByteCircularBuffer)calloc(1, sizeof(struct CircularBuffer));
	ByteCircularBuffer buffer = &circular_buffer;
	
	if(0 != buffer) {
		//buffer->values = (uint8_t *)calloc(capacity, sizeof(uint8_t));
		buffer->values = values_array;
		buffer->capacity = CAPACITY;
		buffer->head   = 0;
		buffer->tail   = 0;
	}
	
	return buffer;
}

void ByteCircularBuffer_Destroy(ByteCircularBuffer buffer)
{
	if(0 != buffer) {
		free(buffer->values);
		free(buffer);
	}
}

uint8_t ByteCircularBuffer_IsEmpty(ByteCircularBuffer buffer)
{
	uint8_t result;
	
	if(0 == buffer->size)
	{
		result = true;
	}
	else 
	{
		result = false;
	}
			
	return result;
}

void ByteCircularBuffer_Push(ByteCircularBuffer buffer, uint16_t value)
{
	HAL_IRQ_DisableInterrupts();
    push(buffer, value);
	HAL_IRQ_EnableInterrupts();
}

void ByteCircularBuffer_PushFromISR(ByteCircularBuffer buffer, uint16_t value)
{
	push(buffer, value);
}

uint16_t ByteCircularBuffer_Pop(ByteCircularBuffer buffer)
{
	uint8_t result;
	
	HAL_IRQ_DisableInterrupts();
	result = pop(buffer);	
	HAL_IRQ_EnableInterrupts();
	
	return result;
}

uint16_t ByteCircularBuffer_PopFromISR(ByteCircularBuffer buffer)
{		
	uint8_t result;
	
	result = pop(buffer);
	
	return result;
}

uint16_t ByteCircularBuffer_GetSize(ByteCircularBuffer buffer)
{
	return buffer->size;
}

static uint16_t pop(ByteCircularBuffer buffer)
{
	uint8_t result;
	
	if(ByteCircularBuffer_IsEmpty(buffer)) 
	{
		result = zeroValue;
	}	
	else
	{
		result = getValue(buffer);
		decreaseSize(buffer);
	}
	
	return result;
}

static void push(ByteCircularBuffer buffer, uint16_t value)
{
	putValue(buffer, value);
	increaseSize(buffer);
}

static void increaseSize(ByteCircularBuffer buffer)
{
	if(buffer->size == buffer->capacity)
		return;
	
	++buffer->size;
}

static void decreaseSize(ByteCircularBuffer buffer)
{
	if(buffer->size == 0)
		return;
	
	--buffer->size;
}

static void resetHeadIndexIfReachedBound(ByteCircularBuffer buffer)
{
	if(buffer->head == buffer->capacity)
		buffer->head = 0;
}

static void putValue(ByteCircularBuffer buffer, uint16_t value)
{
	resetHeadIndexIfReachedBound(buffer);
	buffer->values[buffer->head++] = value;
}

static uint16_t getValue(ByteCircularBuffer buffer)
{
	resetTailIndexIfReachedBound(buffer);
	return buffer->values[buffer->tail++];
}

static void resetTailIndexIfReachedBound(ByteCircularBuffer buffer)
{
	if(buffer->tail == buffer->capacity)
		buffer->tail = 0;
}
