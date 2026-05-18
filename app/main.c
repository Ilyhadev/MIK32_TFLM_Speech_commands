void *__dso_handle __attribute__((weak));

#include "dma_config.h"
#include "mik32_hal_adc.h"
#include "mik32_hal_dma.h"
#include "mik32_hal_usart.h"
#include "mik32_hal_timer32.h"
#include "mik32_hal_scr1_timer.h"
#include "timer32.h"
#include "xprintf.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "spectrogram_kiss.h"
#include "tflm_wrapper.h"

#define ADC_SAMPLE_RATE_HZ 16000U
#define ADC_DMA_PACE_TIMER TIMER32_1
#define ADC_DMA_PACE_REQUEST DMA_CHANNEL_TIMER32_1_REQUEST
#define LOOP_LOG_INTERVAL 20U
/** Match SK_FRAME_HOP: one DMA block = one 20 ms hop when capture keeps up. */
#define ADC_DMA_HALFWORDS SK_FRAME_HOP
#define ML_LOG_SCORES_MAX 6U
/** Must match app/CMakeLists.txt LINKER:__stack_size. */
#define APP_STACK_BYTES 1280U
/** Two 20 ms blocks while quiet: ADC midpoint + spectrogram pad column. */
#define QUIET_CAL_BLOCKS 2U

static void SystemClock_Config(void);
static void USART_Init(void);
static void TMR_Init(void);
static void ADC_Init(void);
static void DMA_Init(void);
static void configure_adc_timer_dma(DMA_ChannelHandleTypeDef *ch);
static uint32_t adc_pace_timer_hz(void);
static uint32_t adc_dma_block_timeout_us(void);
static uint32_t adc_dma_expected_cap_us(void);
static HAL_StatusTypeDef adc_dma_capture_block(void);
static void boot_ram_report(void);
static void ml_log_result(int inv);
static void adc_buffer_minmax(uint16_t *out_min, uint16_t *out_max);
static uint16_t adc_capture_quiet_and_cal(SpectrogramKissState *sk);

static USART_HandleTypeDef husart0;
static ADC_HandleTypeDef hadc;
static DMA_InitTypeDef hdma;
static TIMER32_HandleTypeDef htimer;
static uint16_t adc_buffer[ADC_DMA_HALFWORDS];
static DMA_ChannelHandleTypeDef hdma_ch_adc;
static SpectrogramKissState g_sk;

static volatile uint32_t g_adc_dma_done;

extern char __bss_start[];
extern char __bss_end[];
extern char __data_start[];
extern char __data_end[];
extern char __sp;

static const char *kwd_label(int cls) {
    static const char *const lab[] = {"silence", "unknown", "yes", "no"};
    if (cls < 0 || cls > 3) {
        return "?";
    }
    return lab[cls];
}

static void boot_ram_report(void) {
    const uintptr_t ram_end = 0x02000000UL + 16384UL;
    const size_t data_sz = (size_t)(__data_end - __data_start);
    const size_t bss_sz = (size_t)(__bss_end - __bss_start);

    const uintptr_t stack_hi = (uintptr_t)&__sp;
    const uintptr_t stack_lo = stack_hi - APP_STACK_BYTES;
    const long gap_bss_stack = (long)stack_lo - (long)(uintptr_t)__bss_end;
    const long stack_reserve = (long)((uintptr_t)__bss_end + APP_STACK_BYTES <= stack_hi);

    xprintf("[RAM] data=%u bss=%u free=%ld gap=%ld stack_ok=%ld / 16384\r\n", (unsigned)data_sz,
            (unsigned)bss_sz, (long)(ram_end - (uintptr_t)__bss_end), gap_bss_stack, stack_reserve);
    xprintf("[RAM] g_sk=%u adc=%u arena=%lu/%lu fft=%u\r\n", (unsigned)sizeof(g_sk),
            (unsigned)sizeof(adc_buffer), (unsigned long)tflm_arena_used_bytes(),
            (unsigned long)tflm_arena_size_bytes(), (unsigned)spectrogram_kiss_fft_mem_used(&g_sk));
}

int main(void) {
    SystemClock_Config();
    USART_Init();
    HAL_Time_SCR1TIM_Init();

    ADC_Init();
    DMA_Init();
    TMR_Init();
    configure_adc_timer_dma(&hdma_ch_adc);

    HAL_Timer32_Start(&htimer);

    spectrogram_kiss_init(&g_sk);
    if (!spectrogram_kiss_fft_ready(&g_sk)) {
        xprintf("[BOOT] FAIL: spectrogram init\r\n");
        while (1) {
        }
    }

    xprintf("[BOOT] feat=micro_speech-frontend\r\n");
    xprintf("[BOOT] quiet %u blk for ADC + pad cal...\r\n", (unsigned)QUIET_CAL_BLOCKS);
    spectrogram_kiss_set_cal(&g_sk, 2048U, 2048);
    const uint16_t adc_mid = adc_capture_quiet_and_cal(&g_sk);
    spectrogram_kiss_set_cal(&g_sk, adc_mid, 2048);
    if (spectrogram_kiss_pad_ready(&g_sk)) {
        int8_t pmin = 127;
        int8_t pmax = -128;
        for (unsigned i = 0; i < SK_BINS; i++) {
            const int8_t v = g_sk.pad_column[i];
            if (v < pmin) {
                pmin = v;
            }
            if (v > pmax) {
                pmax = v;
            }
        }
        xprintf("[BOOT] pad_col=%d..%d\r\n", (int)pmin, (int)pmax);
    }
    xprintf("[BOOT] MAX9814 mid=%u gain=8x pad=%s\r\n", (unsigned)adc_mid,
            spectrogram_kiss_pad_ready(&g_sk) ? "cal" : "default");

    if (tflm_init() != 0) {
        xprintf("[BOOT] FAIL: tflm_init err=%d\r\n", tflm_last_error());
        while (1) {
        }
    }

    size_t in_sz = 0;
    int8_t *input_tensor = (int8_t *)tflm_input_buffer(&in_sz);
    if (input_tensor == NULL || in_sz != (size_t)SK_FEATURE_COUNT) {
        xprintf("[BOOT] FAIL: input sz=%lu\r\n", (unsigned long)in_sz);
        while (1) {
        }
    }

    boot_ram_report();
    xprintf("micro_speech: blk=%u @ %u Hz (1st ML ~%us @ ~250ms/blk)\r\n", (unsigned)ADC_DMA_HALFWORDS,
            (unsigned)ADC_SAMPLE_RATE_HZ, (unsigned)(SK_SLICES * 250U / 1000U));
    xprintf("[BOOT] enter loop\r\n");

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
        spectrogram_kiss_push_adc_block(&g_sk, adc_buffer, ADC_DMA_HALFWORDS, input_tensor);
        const uint32_t proc_us = HAL_Time_SCR1TIM_Micros() - proc0;

        // for (int i = 0; i < ADC_DMA_HALFWORDS; i ++) {
        //     xprintf("%d: %d;\r\n", i, adc_buffer[i]);
        // }
        if ((blocks % LOOP_LOG_INTERVAL) == 0U) {
            uint16_t adc_min = 0;
            uint16_t adc_max = 0;
            int8_t feat_min = 0;
            int8_t feat_max = 0;
            adc_buffer_minmax(&adc_min, &adc_max);
            const uint16_t sc = spectrogram_kiss_slice_count(&g_sk);
            if (sc > 0U) {
                const int8_t *col = (const int8_t *)input_tensor + ((uint32_t)(sc - 1U) * SK_BINS);
                feat_min = 127;
                feat_max = -128;
                for (uint16_t i = 0; i < SK_BINS; i++) {
                    const int8_t v = col[i];
                    if (v < feat_min) {
                        feat_min = v;
                    }
                    if (v > feat_max) {
                        feat_max = v;
                    }
                }
            }
            const uint32_t now_us = HAL_Time_SCR1TIM_Micros();
            const uint32_t span_us = now_us - last_log_us;
            last_log_us = now_us;
            const uint32_t blk_per_s_x10 =
                (span_us > 0U) ? (LOOP_LOG_INTERVAL * 10000000UL / span_us) : 0U;
            const uint32_t wall_hz = (blk_per_s_x10 * ADC_DMA_HALFWORDS) / 10U;
            const uint32_t dma_hz =
                (cap_us > 0U) ? ((uint32_t)ADC_DMA_HALFWORDS * 1000000UL / cap_us) : 0U;

            xprintf("[LOOP] blk=%lu cap=%lu proc=%lu exp_cap=%lu slices=%u\r\n", (unsigned long)blocks,
                    (unsigned long)cap_us, (unsigned long)proc_us, (unsigned long)adc_dma_expected_cap_us(),
                    (unsigned)spectrogram_kiss_slice_count(&g_sk));
            xprintf("[LOOP] dma_hz=%lu wall_hz=%lu adc=%u..%u feat=%d..%d\r\n",
                    (unsigned long)dma_hz, (unsigned long)wall_hz, (unsigned)adc_min, (unsigned)adc_max,
                    (int)feat_min, (int)feat_max);
        }

        const uint32_t fseq = spectrogram_kiss_frame_seq(&g_sk);
        const uint16_t sc = spectrogram_kiss_slice_count(&g_sk);
        /* One new spectrogram slice per 320 samples (20 ms @ 16 kHz); skip duplicate fseq. */
        if (spectrogram_kiss_ready(&g_sk) && fseq > last_infer_frame_seq) {
            last_infer_frame_seq = fseq;
            ml_invocations++;
            xprintf("[ML] invoke #%lu fseq=%lu slices=%u, time=%lu\r\n", (unsigned long)ml_invocations,
                    (unsigned long)fseq, (unsigned)sc, (unsigned long)HAL_Time_SCR1TIM_Micros());

            /* input_tensor already holds 49×40 when sc==SK_SLICES (no partial padding). */
            if (ml_invocations <= 5U) {
                int8_t col_min = 127;
                int8_t col_max = -128;
                const int8_t *live = (const int8_t *)input_tensor +
                                     ((sc > 0U) ? ((uint32_t)(sc - 1U) * SK_BINS) : 0U);
                for (uint16_t i = 0; i < SK_BINS; i++) {
                    const int8_t v = live[i];
                    if (v < col_min) {
                        col_min = v;
                    }
                    if (v > col_max) {
                        col_max = v;
                    }
                }
                xprintf("[ML] live_col=%d..%d pad0=%d\r\n", (int)col_min, (int)col_max,
                        (int)g_sk.pad_column[0]);
            }

            const uint32_t inv0 = HAL_Time_SCR1TIM_Micros();
            if (tflm_invoke() != 0) {
                xprintf("[ML] invoke failed err=%d\r\n", tflm_last_error());
                continue;
            }
            const uint32_t inv_us = HAL_Time_SCR1TIM_Micros() - inv0;
            ml_log_result(ml_invocations);
            if (ml_invocations <= 10U) {
                xprintf("[ML] inv_us=%lu proc_us=%lu\r\n", (unsigned long)inv_us, (unsigned long)proc_us);
            }
        }
    }
}

static uint16_t adc_capture_quiet_and_cal(SpectrogramKissState *sk) {
    uint32_t sum = 0U;
    unsigned n = 0U;

    spectrogram_kiss_begin_pad_cal(sk);
    for (unsigned b = 0; b < QUIET_CAL_BLOCKS; b++) {
        if (adc_dma_capture_block() != HAL_OK) {
            break;
        }
        for (unsigned i = 0; i < ADC_DMA_HALFWORDS; i++) {
            sum += adc_buffer[i];
            n++;
        }
        spectrogram_kiss_push_adc_block(sk, adc_buffer, ADC_DMA_HALFWORDS, NULL);
    }
    (void)spectrogram_kiss_finish_pad_cal(sk);

    if (n == 0U) {
        return 2048U;
    }
    return (uint16_t)(sum / n);
}

static void adc_buffer_minmax(uint16_t *out_min, uint16_t *out_max) {
    uint16_t lo = 0xFFFFU;
    uint16_t hi = 0U;
    for (unsigned i = 0; i < ADC_DMA_HALFWORDS; i++) {
        const uint16_t v = adc_buffer[i];
        if (v < lo) {
            lo = v;
        }
        if (v > hi) {
            hi = v;
        }
    }
    if (out_min != NULL) {
        *out_min = lo;
    }
    if (out_max != NULL) {
        *out_max = hi;
    }
}

static void ml_log_result(int inv) {
    const int cls = tflm_get_result();
    xprintf("[ML] #%d -> %s\r\n", inv, kwd_label(cls));
    if ((uint32_t)inv <= ML_LOG_SCORES_MAX) {
        tflm_log_output_scores();
    }
}

static uint32_t adc_dma_expected_cap_us(void) {
    return (ADC_DMA_HALFWORDS * 1000000UL) / ADC_SAMPLE_RATE_HZ;
}

static HAL_StatusTypeDef adc_dma_capture_block(void) {
    const uint32_t dma_len_bytes = (uint32_t)(ADC_DMA_HALFWORDS * sizeof(uint16_t)) - 1U;

    g_adc_dma_done = 0U;
    ADC_DMA_PACE_TIMER->INT_CLEAR = 0xFFFFFFFFu;
    HAL_DMA_Start(&hdma_ch_adc, (void *)&ANALOG_REG->ADC_VALUE, adc_buffer, dma_len_bytes);

    uint32_t saw_busy = 0U;
    const uint32_t deadline_us = HAL_Time_SCR1TIM_Micros() + adc_dma_block_timeout_us();
    while (g_adc_dma_done == 0U) {
        const int ready = HAL_DMA_GetChannelReadyStatus(&hdma_ch_adc);
        if (ready == 0) {
            saw_busy = 1U;
        } else if (saw_busy != 0U) {
            g_adc_dma_done = 1U;
            break;
        }

        if (HAL_Time_SCR1TIM_Micros() >= deadline_us) {
            xprintf("[DMA] timeout\r\n");
            return HAL_TIMEOUT;
        }
    }
    return HAL_OK;
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
