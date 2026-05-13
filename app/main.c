
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
#include "riscv-irq.h"

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
DMA_ChannelHandleTypeDef hdma_ch_adc;

TIMER32_HandleTypeDef htimer;

// Debug counters
volatile uint32_t dma_interrupt_count = 0;
volatile uint32_t dma_spurious_interrupts = 0;

void TMR_Init();
static void ADC_Init(void);
static void DMA_Init(void);
static void configure_interrupts();
void EPIC_trap_handler();
uint16_t adc_buffer[ADC_BUFFER_SIZE];
DMA_ChannelHandleTypeDef hdma_ch_adc_to_mem;

int main() {
    SystemClock_Config();
    USART_Init();
    xprintf("0\r\n");
    TMR_Init();
    xprintf("1\r\n");
    ADC_Init();
    xprintf("2\r\n");
    DMA_Init();
    xprintf("3\r\n");
    
    xprintf("DMA ADC 16kHz sampling started (polling mode)\r\n");
    
    uint32_t last_count = 0;
    int last_ready = 0;
    
    while (1) {
        // // Poll the DMA channel
        // int is_ready = HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc);
        // int has_irq = HAL_DMA_GetChannelIrq(&hdma_ch_adc);
        
        // // When DMA transfer completes, both ready and IRQ should be set
        // if (has_irq && !last_ready) {
        //     dma_interrupt_count++;
            
        //     xprintf("Buffer complete #%lu: Sample[0]=%u Ready=%d IRQ=%d\r\n", 
        //             dma_interrupt_count, adc_buffer[0], is_ready, has_irq);
            
        //     // Restart DMA for circular operation
        //     HAL_DMA_ClearLocalIrq(&hdma);
        //     HAL_DMA_Start(&hdma_ch_adc, 
        //                   (void*)&ANALOG_REG->ADC_VALUE,
        //                   (void*)adc_buffer,
        //                   ADC_BUFFER_SIZE - 1);
        // }
        
        // last_ready = is_ready;

        xprintf("%d\n\r", TIMER32_0->VALUE);
        // HAL_DelayMs(100);
    }
}

void EPIC_trap_handler() {
    int dma_irq_status = HAL_DMA_GetChannelIrq(&hdma_ch_adc);
    int channel_ready = HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc);
    int bus_error = HAL_DMA_GetBusError(&hdma_ch_adc);
    xprintf("sda");
    if (dma_irq_status) {
        dma_interrupt_count++;
        
        HAL_DMA_ClearLocalIrq(&hdma);
        
        EPIC->CLEAR = 1 << EPIC_LINE_DMA_S;
        
        HAL_DMA_Start(&hdma_ch_adc, 
                      (void*)&ANALOG_REG->ADC_VALUE,
                      (void*)adc_buffer,
                      ADC_BUFFER_SIZE - 1);
    } else {
        // Spurious interrupt - no DMA interrupt pending
        dma_spurious_interrupts++;
        
        if (dma_spurious_interrupts <= 5) {
            xprintf("SPURIOUS IRQ! Ready:%d BusErr:%d IRQ:%d Raw:0x%lx\r\n", 
                    channel_ready, bus_error, dma_irq_status, EPIC->RAW_STATUS);
        }
        HAL_DMA_ClearLocalIrq(&hdma);
        EPIC->CLEAR = 1 << EPIC_LINE_DMA_S;
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
    htimer.Clock.Source = TIMER32_SOURCE_PRESCALER;  // Use AHB clock
    htimer.CountMode = TIMER32_COUNTMODE_FORWARD;
    htimer.State = TIMER32_STATE_ENABLE;
    htimer.InterruptMask = 0;         // NO CPU interrupt - DMA handles this
    
    HAL_Timer32_Init(&htimer);
    HAL_Timer32_Start(&htimer);
}

static void ADC_Init(void) {
    hadc.Instance = ANALOG_REG;
    hadc.Init.Sel = ADC_CHANNEL0;
    hadc.Init.EXTRef = ADC_EXTREF_OFF;
    hadc.Init.EXTClb = ADC_EXTCLB_CLBREF;
    
    HAL_ADC_Init(&hadc);

    HAL_ADC_Single(&hadc);
}
void DMA_Init() {
    hdma.Instance = DMA_CONFIG;
    hdma.CurrentValue = DMA_CURRENT_VALUE_DISABLE;
    HAL_DMA_Init(&hdma);

    HAL_DMA_ClearIrq(&hdma);

    HAL_DMA_GlobalIRQEnable(&hdma, DMA_IRQ_ENABLE);
    
    hdma_ch_adc.dma = &hdma;
    hdma_ch_adc.ChannelInit.Channel = DMA_CHANNEL_1;
    hdma_ch_adc.ChannelInit.Priority = DMA_CHANNEL_PRIORITY_HIGH;
    
    hdma_ch_adc.ChannelInit.ReadMode = DMA_CHANNEL_MODE_PERIPHERY;
    hdma_ch_adc.ChannelInit.ReadInc = DMA_CHANNEL_INC_DISABLE;  // Fixed address
    hdma_ch_adc.ChannelInit.ReadSize = DMA_CHANNEL_SIZE_HALFWORD;  // 16-bit ADC
    hdma_ch_adc.ChannelInit.ReadBurstSize = 0;
    hdma_ch_adc.ChannelInit.ReadRequest = DMA_CHANNEL_TIMER32_0_REQUEST;
    hdma_ch_adc.ChannelInit.ReadAck = DMA_CHANNEL_ACK_ENABLE;

    hdma_ch_adc.ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
    hdma_ch_adc.ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
    hdma_ch_adc.ChannelInit.WriteSize = DMA_CHANNEL_SIZE_HALFWORD;
    hdma_ch_adc.ChannelInit.WriteBurstSize = 0;
    hdma_ch_adc.ChannelInit.WriteRequest = 0;
    hdma_ch_adc.ChannelInit.WriteAck = DMA_CHANNEL_ACK_DISABLE;
    
    HAL_DMA_Start(&hdma_ch_adc, 
                  (void*)&ANALOG_REG->ADC_VALUE,
                  (void*)adc_buffer,
                  ADC_BUFFER_SIZE - 1);
    
    // Wait a moment for DMA to be ready
    for (volatile int i = 0; i < 10000; i++) asm("nop");
    
    int ready = HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc);
    int irq = HAL_DMA_GetChannelIrq(&hdma_ch_adc);
    int err = HAL_DMA_GetBusError(&hdma_ch_adc);
    
    xprintf("DMA Start: Ready=%d IRQ=%d BusErr=%d\r\n", ready, irq, err);
    xprintf("DMA Channel CFG before: 0x%08lx\r\n", hdma.Instance->CHANNELS[1].CFG);
    xprintf("DMA Status: 0x%08lx\r\n", hdma.Instance->CONFIG_STATUS);
    
    xprintf("DMA configured. Channel ready: %d\r\n", HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc));
}

void configure_interrupts() {
    PM->CLK_APB_M_SET = PM_CLOCK_APB_M_EPIC_M;
    EPIC->MASK_LEVEL_SET = 1 << (EPIC_LINE_TIMER32_0_S);

    riscv_irq_set_handler(RISCV_IRQ_MEI, EPIC_trap_handler);
    riscv_irq_enable(RISCV_IRQ_MEI);
    riscv_irq_global_enable();
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
