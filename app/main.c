
void *__dso_handle __attribute__((weak));

#include "epic.h"
#include "mik32_hal_irq.h"
#include "mik32_hal_adc.h"
#include "mik32_hal_dma.h"
#include "mik32_hal_usart.h"
#include "mik32_hal_timer32.h"
#include "scr1_timer.h"
#include "timer32.h"
#include "xprintf.h"
#include "tflm_wrapper.h"
#include "kiss_fft_link_test.h"
#include "circular_buffer.h"

#define SYSTEM_FREQ_HZ 32000000UL

#define ADC_RAW_VALUE_MAX                4095U
#define LOOP_DELAY_MS                    800U
#define SAMPLE_FREQ_HZ (100)
#define BLINK_PERIOD_TICKS (SYSTEM_FREQ_HZ / SAMPLE_FREQ_HZ)
#define ADC_BUFFER_SIZE 256

static void SystemClock_Config();
static void USART_Init();

static USART_HandleTypeDef husart0;
ADC_HandleTypeDef hadc;
DMA_InitTypeDef hdma;

TIMER32_HandleTypeDef htimer;

// Debug counters
volatile uint32_t dma_interrupt_count = 0;
volatile uint32_t dma_spurious_interrupts = 0;

void TMR_Init();
static void ADC_Init(void);
static void DMA_Init(void);
static void configure_interrupts();

void configure_mem_to_mem_dma(DMA_InitTypeDef* hdma, DMA_ChannelHandleTypeDef* ch);
uint16_t adc_buffer[ADC_BUFFER_SIZE];
DMA_ChannelHandleTypeDef hdma_ch_mem_to_mem;

int main() {
    SystemClock_Config();
    USART_Init();
    xprintf("\r\n--- System Boot ---\r\n");
    
    ADC_Init();
    DMA_Init();
    TMR_Init();
    __HAL_PCC_EPIC_CLK_ENABLE();
    EPIC->MASK_LEVEL_SET = 0xFFFFFFFF;//1 << (EPIC_LINE_TIMER32_0_S);
    HAL_Timer32_PWM_Start_IT(&htimer, );
    HAL_IRQ_EnableInterrupts();

    configure_mem_to_mem_dma(&hdma, &hdma_ch_mem_to_mem);

    // MIK32 HAL requires length in BYTES minus 1.
    uint32_t dma_len = (ADC_BUFFER_SIZE * sizeof(uint16_t)) - 1;
    uint32_t buffer_count = 0;

    while (1) {
        // xprintf("%d\n\r",TIMER32_0->VALUE);
        HAL_DMA_Start(&hdma_ch_mem_to_mem, 
                      (void*)&ANALOG_REG->ADC_VALUE, 
                      adc_buffer, 
                      200);
        if (HAL_DMA_Wait(&hdma_ch_mem_to_mem, 1000) == HAL_OK) {
            
            buffer_count++;
            
            xprintf("Buffer #%lu full! First: %u, Last: %u\r\n", 
                    buffer_count, adc_buffer[0], adc_buffer[ADC_BUFFER_SIZE - 1]);
            
            for(volatile int i=0; i<1000; i++); 
            
        } else {
            xprintf("DMA Timeout! Transfer got stuck.\r\n");
        }

    }
}

void trap_handler() {
    xprintf("dsa");
    if (EPIC->RAW_STATUS & (1 << EPIC_LINE_TIMER32_0_S)) {
        TIMER32_0->INT_CLEAR = TIMER32_INT_OVERFLOW_M;
         EPIC->CLEAR = EPIC_LINE_TIMER32_0_S;
    }
}


void SystemClock_Config(void)
{
    PCC_InitTypeDef PCC_OscInit = {0};

    PCC_OscInit.OscillatorEnable = PCC_OSCILLATORTYPE_ALL;
    PCC_OscInit.FreqMon.OscillatorSystem = PCC_OSCILLATORTYPE_OSC32M;
    PCC_OscInit.FreqMon.ForceOscSys = PCC_FORCE_OSC_SYS_UNFIXED;
    PCC_OscInit.FreqMon.Force32KClk = PCC_FREQ_MONITOR_SOURCE_OSC32K;
    PCC_OscInit.AHBDivider = 0;
    PCC_OscInit.APBMDivider = 0;
    PCC_OscInit.APBPDivider = 0;
    PCC_OscInit.HSI32MCalibrationValue = 128;
    PCC_OscInit.LSI32KCalibrationValue = 8;
    PCC_OscInit.RTCClockSelection = PCC_RTC_CLOCK_SOURCE_AUTO;
    PCC_OscInit.RTCClockCPUSelection = PCC_CPU_RTC_CLOCK_SOURCE_OSC32K;
    HAL_PCC_Config(&PCC_OscInit);
}

void TMR_Init() {
    PM->CLK_APB_M_SET = PM_CLOCK_APB_M_TIMER32_0_M;
    
    htimer.Instance = TIMER32_0;
    htimer.Top = 2000;
    htimer.Clock.Prescaler = 0;
    htimer.Clock.Source = TIMER32_SOURCE_PRESCALER;
    htimer.CountMode = TIMER32_COUNTMODE_FORWARD;
    htimer.State = TIMER32_STATE_ENABLE;
    htimer.InterruptMask = TIMER32_INT_OVERFLOW_M;

    HAL_Timer32_Init(&htimer);
    HAL_Timer32_Start(&htimer);
}

static void ADC_Init(void) {
    hadc.Instance = ANALOG_REG;
    hadc.Init.Sel = ADC_CHANNEL0;
    hadc.Init.EXTRef = ADC_EXTREF_OFF; // Use internal 1.2V reference
    hadc.Init.EXTClb = ADC_EXTCLB_CLBREF;
    
    HAL_ADC_Init(&hadc);

    HAL_ADC_ContinuousEnable(&hadc);
}

void configure_mem_to_mem_dma(DMA_InitTypeDef* hdma, DMA_ChannelHandleTypeDef* ch) {
    ch->dma = hdma;

    /* Настройки канала */
    ch->ChannelInit.Channel = DMA_CHANNEL_1;
    ch->ChannelInit.Priority = DMA_CHANNEL_PRIORITY_VERY_HIGH;

    ch->ChannelInit.ReadMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.ReadInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.ReadSize = DMA_CHANNEL_SIZE_BYTE; /* data_len должно быть кратно read_size */
    ch->ChannelInit.ReadBurstSize = 0;                /* read_burst_size должно быть кратно read_size */
    ch->ChannelInit.ReadRequest = 0;  //DMA_CHANNEL_USART_0_REQUEST;
    ch->ChannelInit.ReadAck = DMA_CHANNEL_ACK_DISABLE;

    ch->ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.WriteSize = DMA_CHANNEL_SIZE_BYTE; /* data_len должно быть кратно write_size */
    ch->ChannelInit.WriteBurstSize = 0;                /* write_burst_size должно быть кратно read_size */
    ch->ChannelInit.WriteRequest = 0; //DMA_CHANNEL_USART_0_REQUEST;
    ch->ChannelInit.WriteAck = DMA_CHANNEL_ACK_ENABLE;
}

void DMA_Init(void) {
    hdma.Instance = DMA_CONFIG;
    hdma.CurrentValue = DMA_CURRENT_VALUE_ENABLE;
    HAL_DMA_Init(&hdma);
}



void configure_interrupts() {
    __HAL_PCC_EPIC_CLK_ENABLE();
    HAL_EPIC_MaskLevelSet(HAL_EPIC_UART_0_MASK);
    // HAL_USART_RXNE_EnableInterrupt(&husart0);
    HAL_IRQ_EnableInterrupts();
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc) {
    GPIO_InitTypeDef GPIO_InitStruct = { 0 };
    __HAL_PCC_ANALOG_REGS_CLK_ENABLE();

    GPIO_InitStruct.Mode = HAL_GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = HAL_GPIO_PULL_NONE;
    GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIO_1, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_7 | GPIO_PIN_9;
    HAL_GPIO_Init(GPIO_0, &GPIO_InitStruct);
}

void USART_Init()
{
    husart0.Instance = UART_0;
    husart0.transmitting = Enable;
    husart0.receiving = Enable;
    husart0.frame = Frame_8bit;
    husart0.parity_bit = Disable;
    husart0.parity_bit_inversion = Disable;
    husart0.bit_direction = LSB_First;
    husart0.data_inversion = Disable;
    husart0.tx_inversion = Disable;
    husart0.rx_inversion = Disable;
    husart0.swap = Disable;
    husart0.lbm = Disable;
    husart0.stop_bit = StopBit_1;
    husart0.mode = Asynchronous_Mode;
    husart0.xck_mode = XCK_Mode3;
    husart0.last_byte_clock = Disable;
    husart0.overwrite = Disable;
    husart0.rts_mode = AlwaysEnable_mode;
    husart0.dma_tx_request = Disable;
    husart0.dma_rx_request = Disable;
    husart0.channel_mode = Duplex_Mode;
    husart0.tx_break_mode = Disable;
    husart0.Interrupt.ctsie = Disable;
    husart0.Interrupt.eie = Disable;
    husart0.Interrupt.idleie = Disable;
    husart0.Interrupt.lbdie = Disable;
    husart0.Interrupt.peie = Disable;
    husart0.Interrupt.rxneie = Disable;
    husart0.Interrupt.tcie = Disable;
    husart0.Interrupt.txeie = Disable;
    husart0.Modem.rts = Disable; //out
    husart0.Modem.cts = Disable; //in
    husart0.Modem.dtr = Disable; //out
    husart0.Modem.dcd = Disable; //in
    husart0.Modem.dsr = Disable; //in
    husart0.Modem.ri = Disable;  //in
    husart0.Modem.ddis = Disable;//out
    husart0.baudrate = 115200;
    HAL_USART_Init(&husart0);
}

















// void DMA_Init() {
//     // Initialize DMA controller
//     hdma.Instance = DMA_CONFIG;
//     hdma.CurrentValue = DMA_CURRENT_VALUE_DISABLE;
//     HAL_DMA_Init(&hdma);
    
//     // Clear any pending interrupts from before
//     HAL_DMA_ClearIrq(&hdma);
    
//     // Enable global DMA interrupt
//     HAL_DMA_GlobalIRQEnable(&hdma, DMA_IRQ_ENABLE);
    
//     // Configure channel
//     hdma_ch_adc.dma = &hdma;
//     hdma_ch_adc.ChannelInit.Channel = DMA_CHANNEL_1;  // Use channel 1
//     hdma_ch_adc.ChannelInit.Priority = DMA_CHANNEL_PRIORITY_HIGH;
    
//     // Source: ADC register (peripheral, no increment)
//     hdma_ch_adc.ChannelInit.ReadMode = DMA_CHANNEL_MODE_PERIPHERY;
//     hdma_ch_adc.ChannelInit.ReadInc = DMA_CHANNEL_INC_DISABLE;  // Fixed address
//     hdma_ch_adc.ChannelInit.ReadSize = DMA_CHANNEL_SIZE_HALFWORD;  // 16-bit ADC
//     hdma_ch_adc.ChannelInit.ReadBurstSize = 0;
//     hdma_ch_adc.ChannelInit.ReadRequest = DMA_CHANNEL_TIMER32_0_REQUEST;
//     // With handshake enabled, DMA waits for each timer pulse (paced transfer)
//     hdma_ch_adc.ChannelInit.ReadAck = DMA_CHANNEL_ACK_ENABLE;
    
//     // Destination: RAM buffer (memory, increment)
//     hdma_ch_adc.ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
//     hdma_ch_adc.ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
//     hdma_ch_adc.ChannelInit.WriteSize = DMA_CHANNEL_SIZE_HALFWORD;
//     hdma_ch_adc.ChannelInit.WriteBurstSize = 0;
//     hdma_ch_adc.ChannelInit.WriteRequest = 0;
//     hdma_ch_adc.ChannelInit.WriteAck = DMA_CHANNEL_ACK_DISABLE;
    
//     // Start continuous DMA transfer
//     HAL_DMA_Start(&hdma_ch_adc, 
//                   (void*)&ANALOG_REG->ADC_VALUE,
//                   (void*)adc_buffer,
//                   ADC_BUFFER_SIZE - 1);
    
//     // Wait a moment for DMA to be ready
//     for (volatile int i = 0; i < 10000; i++) asm("nop");
    
//     int ready = HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc);
//     int irq = HAL_DMA_GetChannelIrq(&hdma_ch_adc);
//     int err = HAL_DMA_GetBusError(&hdma_ch_adc);
    
//     xprintf("DMA Start: Ready=%d IRQ=%d BusErr=%d\r\n", ready, irq, err);
//     xprintf("DMA Channel CFG before: 0x%08lx\r\n", hdma.Instance->CHANNELS[1].CFG);
//     xprintf("DMA Status: 0x%08lx\r\n", hdma.Instance->CONFIG_STATUS);
    
//     // If still not ready, try enabling the channel explicitly
//     if (!ready) {
//         xprintf("Channel not ready! Trying explicit enable...\r\n");
//         HAL_DMA_ChannelEnable(&hdma_ch_adc);
        
//         for (volatile int i = 0; i < 10000; i++) asm("nop");
        
//         ready = HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc);
//         xprintf("After explicit enable: Ready=%d\r\n", ready);
//         HAL_DMA_Start(&hdma_ch_adc, 
//                   (void*)&ANALOG_REG->ADC_VALUE,
//                   (void*)adc_buffer,
//                   ADC_BUFFER_SIZE - 1);
//     }
    
//     xprintf("DMA configured. Channel ready: %d\r\n", HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc));
// }