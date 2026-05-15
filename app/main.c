
void *__dso_handle __attribute__((weak));

#include "dma_config.h"
#include "epic.h"
#include "mik32_hal_irq.h"
#include "mik32_hal_adc.h"
#include "mik32_hal_dma.h"
#include "mik32_hal_usart.h"
#include "mik32_hal_timer32.h"
#include "mik32_hal_pcc.h"
#include "mik32_hal_scr1_timer.h"
#include "mik32_memory_map.h"
#include "power_manager.h"
#include "timer32.h"
#include "xprintf.h"

#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>

static void SystemClock_Config(void);
static void USART_Init(void);

static USART_HandleTypeDef husart0;

#include "tflm_wrapper.h"

#ifndef MICRO_SPEECH_ML_SMOKE
#define MICRO_SPEECH_ML_SMOKE 0
#endif

#if !MICRO_SPEECH_ML_SMOKE
#include "spectrogram_kiss.h"

#define ADC_SAMPLE_RATE_HZ 16000U
/** ~31 DMA IRQ/s @ 16 kHz; fits 16 KiB with shrunk kiss + arena. */
#define ADC_DMA_HALFWORDS 256U
#define ADC_DMA_PACE_TIMER TIMER32_1
#define ADC_DMA_PACE_REQUEST DMA_CHANNEL_TIMER32_1_REQUEST
#define LOOP_LOG_INTERVAL 20U

static void TMR_Init(void);
static void ADC_Init(void);
static void DMA_Init(void);
static void configure_adc_timer_dma(DMA_ChannelHandleTypeDef *ch);
static void dma_irq_init(void);
static uint32_t adc_pace_timer_hz(void);
static uint32_t adc_dma_block_timeout_us(void);
static HAL_StatusTypeDef adc_dma_capture_block(void);

static ADC_HandleTypeDef hadc;
static DMA_InitTypeDef hdma;
static TIMER32_HandleTypeDef htimer;
static uint16_t adc_buffer[ADC_DMA_HALFWORDS];
static DMA_ChannelHandleTypeDef hdma_ch_adc;
static SpectrogramKissState g_sk;

static volatile uint32_t g_adc_dma_done;
static volatile uint32_t g_dma_isr_count;
#else
#define LOOP_LOG_INTERVAL 50U
#endif

extern char __bss_start[];
extern char __bss_end[];
extern char __data_start[];
extern char __data_end[];

static volatile uint32_t g_trap_calls;

static void boot_trace(const char *msg) {
    xprintf("[BOOT] %s\r\n", msg);
}

static void boot_ram_report(void) {
    const uintptr_t ram_base = 0x02000000UL;
    const uintptr_t ram_end = ram_base + 16384UL;
    const size_t data_sz = (size_t)(__data_end - __data_start);
    const size_t bss_sz = (size_t)(__bss_end - __bss_start);
    const long gap = (long)(ram_end - (uintptr_t)__bss_end);

    xprintf("[RAM] data=%u bss=%u free_to_ram_top=%ld / 16384\r\n", (unsigned)data_sz, (unsigned)bss_sz,
            (long)gap);
#if !MICRO_SPEECH_ML_SMOKE
    xprintf("[RAM] g_sk=%u adc=%u arena=%lu/%lu fft_used=%u\r\n", (unsigned)sizeof(g_sk),
            (unsigned)sizeof(adc_buffer), (unsigned long)tflm_arena_used_bytes(),
            (unsigned long)tflm_arena_size_bytes(), (unsigned)spectrogram_kiss_fft_mem_used(&g_sk));
#else
    xprintf("[RAM] arena=%lu/%lu (ML smoke, no DMA/FFT)\r\n", (unsigned long)tflm_arena_used_bytes(),
            (unsigned long)tflm_arena_size_bytes());
#endif
}

static int ml_invoke_logged(uint32_t tag) {
    xprintf("[ML] invoke #%lu (arena free=%lu B) ...\r\n", (unsigned long)tag,
            (unsigned long)tflm_arena_free_bytes());
    const uint32_t t0 = HAL_Time_SCR1TIM_Micros();
    const int err = tflm_invoke();
    const uint32_t dt = HAL_Time_SCR1TIM_Micros() - t0;
    if (err != 0) {
        xprintf("[ML] invoke #%lu FAIL err=%d last=%d dt=%lu us\r\n", (unsigned long)tag, err,
                tflm_last_error(), (unsigned long)dt);
        return err;
    }
    xprintf("[ML] invoke #%lu OK dt=%lu us\r\n", (unsigned long)tag, (unsigned long)dt);
    tflm_log_output_scores();
    return 0;
}

#if MICRO_SPEECH_ML_SMOKE
static int run_ml_smoke_loop(void) {
    size_t in_sz = 0;
    int8_t *input_tensor = (int8_t *)tflm_input_buffer(&in_sz);
    if (input_tensor == NULL || in_sz != 1960U) {
        xprintf("[BOOT] FAIL: input ptr=0x%08lx sz=%lu (want 1960)\r\n",
                (unsigned long)(uintptr_t)input_tensor, (unsigned long)in_sz);
        while (1) {
        }
    }
    memset(input_tensor, 1, in_sz);
    xprintf("[BOOT] ML smoke: filled %lu B with 0x01\r\n", (unsigned long)in_sz);

    boot_trace("5: before invoke #1");
    if (ml_invoke_logged(1U) != 0) {
        xprintf("[BOOT] first invoke failed; halting\r\n");
        while (1) {
        }
    }

    uint32_t inv = 2;
    while (1) {
        for (volatile uint32_t d = 0; d < 5000000U; d++) {
        }
        (void)ml_invoke_logged(inv++);
        if ((inv % LOOP_LOG_INTERVAL) == 0U) {
            xprintf("[LOOP] smoke inv=%lu traps=%lu\r\n", (unsigned long)inv, (unsigned long)g_trap_calls);
        }
    }
    return 0;
}
#endif

void app_trap_handler(void) {
    g_trap_calls++;
#if MICRO_SPEECH_ML_SMOKE
    /* Do not xprintf here: trap stack is only 128 B. */
    return;
#endif

#if !MICRO_SPEECH_ML_SMOKE
    if ((EPIC->RAW_STATUS & HAL_EPIC_DMA_MASK) == 0U) {
        return;
    }

    if (HAL_DMA_GetChannelIrq(&hdma_ch_adc) != 0) {
        g_dma_isr_count++;
        g_adc_dma_done = 1U;
        HAL_DMA_ClearLocalIrq(&hdma);
        HAL_EPIC_Clear(HAL_EPIC_DMA_MASK);
    }
#endif
}

int main(void) {
    SystemClock_Config();
    USART_Init();
    HAL_Time_SCR1TIM_Init();
    boot_trace("0: SystemClock + USART OK");

#if MICRO_SPEECH_ML_SMOKE
    if (tflm_init() != 0) {
        xprintf("[BOOT] FAIL: tflm_init err=%d\r\n", tflm_last_error());
        while (1) {
        }
    }
    boot_trace("1: after tflm_init");
    tflm_log_tensors();
    boot_trace("2: after log_tensors");
    boot_ram_report();
    boot_trace("3: after ram_report");
    xprintf("micro_speech: ML_SMOKE (no DMA/FFT)\r\n");
    boot_trace("4: enter smoke loop");
    (void)run_ml_smoke_loop();
#else
    if (tflm_init() != 0) {
        xprintf("[BOOT] FAIL: tflm_init err=%d\r\n", tflm_last_error());
        while (1) {
        }
    }
    tflm_log_tensors();

    ADC_Init();
    DMA_Init();
    TMR_Init();
    configure_adc_timer_dma(&hdma_ch_adc);
    dma_irq_init();

    HAL_Timer32_Start(&htimer);
    HAL_IRQ_EnableInterrupts();

    spectrogram_kiss_init(&g_sk);
    if (!spectrogram_kiss_fft_ready(&g_sk)) {
        xprintf("[BOOT] FAIL: kiss_fftr_alloc (FFT)\r\n");
        while (1) {
        }
    }

    size_t in_sz = 0;
    int8_t *input_tensor = (int8_t *)tflm_input_buffer(&in_sz);
    if (input_tensor == NULL || in_sz != (size_t)SK_FEATURE_COUNT) {
        xprintf("[BOOT] FAIL: input ptr=0x%08lx sz=%lu want %u\r\n",
                (unsigned long)(uintptr_t)input_tensor, (unsigned long)in_sz, (unsigned)SK_FEATURE_COUNT);
        while (1) {
        }
    }

    boot_ram_report();
    xprintf("micro_speech: blk=%u @ %u Hz, arena %lu/%lu B\r\n", (unsigned)ADC_DMA_HALFWORDS,
            (unsigned)ADC_SAMPLE_RATE_HZ, (unsigned long)tflm_arena_used_bytes(),
            (unsigned long)tflm_arena_size_bytes());
    xprintf("[BOOT] cap ~%lu us/blk expect\r\n",
            (unsigned long)((ADC_DMA_HALFWORDS * 1000000UL) / ADC_SAMPLE_RATE_HZ));
    boot_trace("enter loop");

    uint32_t blocks = 0;
    uint32_t ml_invocations = 0;
    uint32_t last_infer_frame_seq = 0;
    uint32_t last_log_us = HAL_Time_SCR1TIM_Micros();

    while (1) {
        blocks++;

        const uint32_t t0_us = HAL_Time_SCR1TIM_Micros();
        if (adc_dma_capture_block() != HAL_OK) {
            xprintf("[LOOP] DMA timeout blk=%lu\r\n", (unsigned long)blocks);
            continue;
        }
        const uint32_t cap_us = HAL_Time_SCR1TIM_Micros() - t0_us;

        const uint32_t proc0 = HAL_Time_SCR1TIM_Micros();
        for (unsigned i = 0; i < ADC_DMA_HALFWORDS; i++) {
            spectrogram_kiss_push_adc(&g_sk, adc_buffer[i], input_tensor);
        }
        const uint32_t proc_us = HAL_Time_SCR1TIM_Micros() - proc0;

        if (blocks <= 3U || (blocks % LOOP_LOG_INTERVAL) == 0U) {
            const uint32_t now_us = HAL_Time_SCR1TIM_Micros();
            const uint32_t span_us = now_us - last_log_us;
            last_log_us = now_us;
            const uint32_t blk_per_s_x10 =
                (span_us > 0U && blocks > 1U) ? (LOOP_LOG_INTERVAL * 10000000UL / span_us) : 0U;
            const uint32_t hz_audio = (blk_per_s_x10 * ADC_DMA_HALFWORDS) / 10U;

            xprintf("[LOOP] blk=%lu cap=%lu proc=%lu slices=%u ready=%u\r\n", (unsigned long)blocks,
                    (unsigned long)cap_us, (unsigned long)proc_us, (unsigned)spectrogram_kiss_slice_count(&g_sk),
                    spectrogram_kiss_ready(&g_sk) ? 1U : 0U);
            if (blocks > 1U) {
                xprintf("[LOOP] ~%lu.%lu blk/s ~%lu Hz isr=%lu\r\n", (unsigned long)(blk_per_s_x10 / 10U),
                        (unsigned long)(blk_per_s_x10 % 10U), (unsigned long)hz_audio,
                        (unsigned long)g_dma_isr_count);
            }
        }

        const uint32_t fseq = spectrogram_kiss_frame_seq(&g_sk);
        if (spectrogram_kiss_ready(&g_sk) && fseq > last_infer_frame_seq) {
            last_infer_frame_seq = fseq;
            ml_invocations++;
            HAL_IRQ_DisableInterrupts();
            (void)ml_invoke_logged(ml_invocations);
            HAL_IRQ_EnableInterrupts();
        }
    }
#endif
}

#if !MICRO_SPEECH_ML_SMOKE
static HAL_StatusTypeDef adc_dma_capture_block(void) {
    const uint32_t dma_len_bytes = (uint32_t)(ADC_DMA_HALFWORDS * sizeof(uint16_t)) - 1U;

    g_adc_dma_done = 0U;
    ADC_DMA_PACE_TIMER->INT_CLEAR = 0xFFFFFFFFu;
    HAL_DMA_ClearLocalIrq(&hdma);
    HAL_DMA_Start(&hdma_ch_adc, (void *)&ANALOG_REG->ADC_VALUE, adc_buffer, dma_len_bytes);

    const uint32_t deadline_us = HAL_Time_SCR1TIM_Micros() + adc_dma_block_timeout_us();
    while (g_adc_dma_done == 0U) {
        if (HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc) != 0) {
            g_adc_dma_done = 1U;
            break;
        }
        if (HAL_Time_SCR1TIM_Micros() >= deadline_us) {
            return HAL_TIMEOUT;
        }
        __asm volatile("wfi");
    }
    return HAL_OK;
}

static void dma_irq_init(void) {
    __HAL_PCC_EPIC_CLK_ENABLE();
    HAL_EPIC_Clear(HAL_EPIC_DMA_MASK);
    HAL_DMA_ClearIrq(&hdma);

    HAL_DMA_GlobalIRQEnable(&hdma, DMA_IRQ_ENABLE);
    HAL_DMA_ErrorIRQEnable(&hdma, DMA_IRQ_ENABLE);
    HAL_DMA_LocalIRQEnable(&hdma_ch_adc, DMA_IRQ_ENABLE);
    HAL_EPIC_MaskLevelSet(HAL_EPIC_DMA_MASK);
}

static uint32_t adc_pace_timer_hz(void) {
    return HAL_PCC_GetSysClockFreq() / ((PM->DIV_AHB + 1U) * (PM->DIV_APB_P + 1U));
}

static uint32_t adc_dma_block_timeout_us(void) {
    return ((ADC_DMA_HALFWORDS * 1000000UL) / ADC_SAMPLE_RATE_HZ) * 3U;
}

static void TMR_Init(void) {
    const uint32_t pace_hz = adc_pace_timer_hz();
    const uint32_t top = (pace_hz / ADC_SAMPLE_RATE_HZ) - 1U;

    htimer.Instance = ADC_DMA_PACE_TIMER;
    htimer.Top = top;
    htimer.Clock.Prescaler = 0;
    htimer.Clock.Source = TIMER32_SOURCE_TIM1_HCLK;
    htimer.CountMode = TIMER32_COUNTMODE_FORWARD;
    htimer.State = TIMER32_STATE_ENABLE;
    htimer.InterruptMask = 0;

    HAL_Timer32_Init(&htimer);
    ADC_DMA_PACE_TIMER->INT_CLEAR = 0xFFFFFFFF;
    xprintf("[TMR] HCLK %lu Hz TOP=%lu\r\n", (unsigned long)pace_hz, (unsigned long)ADC_DMA_PACE_TIMER->TOP);
}

static void ADC_Init(void) {
    hadc.Instance = ANALOG_REG;
    hadc.Init.Sel = ADC_CHANNEL0;
    hadc.Init.EXTRef = ADC_EXTREF_OFF;
    hadc.Init.EXTClb = ADC_EXTCLB_CLBREF;
    HAL_ADC_Init(&hadc);
    HAL_ADC_ContinuousEnable(&hadc);
}

static void configure_adc_timer_dma(DMA_ChannelHandleTypeDef *ch) {
    ch->dma = &hdma;
    ch->ChannelInit.Channel = DMA_CHANNEL_0;
    ch->ChannelInit.Priority = DMA_CHANNEL_PRIORITY_HIGH;
    ch->ChannelInit.ReadMode = DMA_CHANNEL_MODE_PERIPHERY;
    ch->ChannelInit.ReadInc = DMA_CHANNEL_INC_DISABLE;
    ch->ChannelInit.ReadSize = DMA_CHANNEL_SIZE_HALFWORD;
    ch->ChannelInit.ReadBurstSize = 1U;
    ch->ChannelInit.ReadRequest = ADC_DMA_PACE_REQUEST;
    ch->ChannelInit.ReadAck = DMA_CHANNEL_ACK_ENABLE;
    ch->ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.WriteSize = DMA_CHANNEL_SIZE_HALFWORD;
    ch->ChannelInit.WriteBurstSize = 1U;
    ch->ChannelInit.WriteRequest = 0;
    ch->ChannelInit.WriteAck = DMA_CHANNEL_ACK_DISABLE;
}

static void DMA_Init(void) {
    hdma.Instance = DMA_CONFIG;
    hdma.CurrentValue = DMA_CURRENT_VALUE_ENABLE;
    HAL_DMA_Init(&hdma);
}
#endif

void SystemClock_Config(void) {
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

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc) {
    (void)hadc;
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    __HAL_PCC_ANALOG_REGS_CLK_ENABLE();

    GPIO_InitStruct.Mode = HAL_GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = HAL_GPIO_PULL_NONE;
    GPIO_InitStruct.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    HAL_GPIO_Init(GPIO_1, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_2 | GPIO_PIN_4 | GPIO_PIN_7 | GPIO_PIN_9;
    HAL_GPIO_Init(GPIO_0, &GPIO_InitStruct);
}

static void USART_Init(void) {
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
    husart0.Modem.rts = Disable;
    husart0.Modem.cts = Disable;
    husart0.Modem.dtr = Disable;
    husart0.Modem.dcd = Disable;
    husart0.Modem.dsr = Disable;
    husart0.Modem.ri = Disable;
    husart0.Modem.ddis = Disable;
    husart0.baudrate = 115200;
    HAL_USART_Init(&husart0);
}
