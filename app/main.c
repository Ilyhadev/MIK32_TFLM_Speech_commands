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

#include "adc_ram_capture.h"
#include "spectrogram_kiss.h"
#include "tflm_wrapper.h"

#define SAMPLE_RATE_HZ 16000U
#define DMA_TIMER TIMER32_1
#define DMA_REQ DMA_CHANNEL_TIMER32_1_REQUEST
#define DMA_SAMPLES (SK_FRAME_HOP * 2U)
#define HOPS_PER_DMA_BLOCK (DMA_SAMPLES / SK_FRAME_HOP)
#define MAX_LIVE_HOPS 16U
#define LOOP_LOG_EVERY 20U
#define QUIET_BLOCKS 20U
#define TRIGGER_PK 300U
#define MIC_GAIN_Q8 48

static USART_HandleTypeDef husart0;
static ADC_HandleTypeDef hadc;
static DMA_InitTypeDef hdma;
static TIMER32_HandleTypeDef htimer;
static DMA_ChannelHandleTypeDef hdma_ch;
static uint16_t dma_buf[DMA_SAMPLES];
static SpectrogramKissState sk;
static int8_t feat_live[MAX_LIVE_HOPS * SK_BINS];
static uint8_t shot_fft_stash_blk;
static bool shot_fft_stashed;

static volatile uint32_t dma_done;
static uint16_t adc_mid = 1630U;
static uint16_t pk_win_max;

static const char *cls_name(int c) {
    static const char *const n[] = {"silence", "unknown", "yes", "no"};
    if (c == -1) {
        return "ambiguous";
    }
    return (c >= 0 && c <= 3) ? n[c] : "?";
}

static void clock_init(void);
static void usart_init(void);
static void timer_init(void);
static void adc_init(void);
static void dma_init(void);
static void dma_ch_config(DMA_ChannelHandleTypeDef *ch);
static uint32_t timer_hz(void);
static HAL_StatusTypeDef dma_wait_block(void);
static uint16_t block_peak(const uint16_t *s, unsigned n);
static uint16_t quiet_cal(void);
static void warm_noise_quiet(void);
static unsigned tensor_saturated_bins(const int8_t *tensor);
static HAL_StatusTypeDef record_shot(void);
static void shot_stash_fft_blocks(unsigned n_blk);
static const uint16_t *shot_pcm_block(unsigned block_index);
static void log_model_input(const int8_t *tensor);
static void process_shot(void);

int main(void) {
    clock_init();
    usart_init();
    HAL_Time_SCR1TIM_Init();
    adc_init();
    dma_init();
    timer_init();
    dma_ch_config(&hdma_ch);
    HAL_Timer32_Start(&htimer);

    spectrogram_kiss_init(&sk);
    if (!spectrogram_kiss_fft_ready(&sk)) {
        xprintf("[BOOT] spectrogram init failed\r\n");
        for (;;) {
        }
    }

    xprintf("[BOOT] quiet cal %u blocks...\r\n", (unsigned)QUIET_BLOCKS);
    adc_mid = quiet_cal();
    spectrogram_kiss_set_cal(&sk, adc_mid, MIC_GAIN_Q8);
    spectrogram_kiss_begin_pad_cal(&sk);
    warm_noise_quiet();
    if (!spectrogram_kiss_finish_pad_cal(&sk)) {
        spectrogram_kiss_set_default_pad(&sk);
        xprintf("[BOOT] pad cal fallback default\r\n");
    }
    warm_noise_quiet();
    spectrogram_kiss_reset_stream(&sk);

    adc_ram_capture_reset();
    const unsigned cap_blk =
        (unsigned)((adc_ram_capture_pool_bytes() + ADC_RAM_CAPTURE_BLOCK_BYTES) /
                   ADC_RAM_CAPTURE_BLOCK_BYTES);
    xprintf("[BOOT] mid=%u gain=%u trigger pk>=%u\r\n", (unsigned)adc_mid, (unsigned)MIC_GAIN_Q8,
            (unsigned)TRIGGER_PK);
    xprintf("[BOOT] dma_blk=%u B capture=%u B (~%u blk w/ fft_mem) arena=%u B\r\n",
            (unsigned)sizeof(dma_buf), (unsigned)adc_ram_capture_pool_bytes(), cap_blk,
            (unsigned)tflm_arena_size_bytes());
    xprintf("[BOOT] DMA %u Hz %u smp/blk ~40ms (~25 blk/s idle)\r\n", (unsigned)SAMPLE_RATE_HZ,
            (unsigned)DMA_SAMPLES);

    uint32_t blocks = 0;
    uint32_t log_t0 = HAL_Time_SCR1TIM_Micros();

    for (;;) {
        blocks++;
        if (dma_wait_block() != HAL_OK) {
            continue;
        }

        const uint16_t pk = block_peak(dma_buf, DMA_SAMPLES);
        if (pk > pk_win_max) {
            pk_win_max = pk;
        }

        if (pk >= TRIGGER_PK) {
            static uint32_t cooldown;
            if (cooldown > 0U) {
                cooldown--;
                continue;
            }
            xprintf("[SHOT] pk=%u record\r\n", (unsigned)pk);
            if (record_shot() == HAL_OK) {
                process_shot();
                log_t0 = HAL_Time_SCR1TIM_Micros();
                cooldown = 80U;
            } else {
                xprintf("[SHOT] record failed\r\n");
            }
            continue;
        }

        if ((blocks % LOOP_LOG_EVERY) == 0U) {
            uint16_t lo = 0xFFFFU;
            uint16_t hi = 0U;
            for (unsigned i = 0U; i < DMA_SAMPLES; i++) {
                if (dma_buf[i] < lo) {
                    lo = dma_buf[i];
                }
                if (dma_buf[i] > hi) {
                    hi = dma_buf[i];
                }
            }
            const uint32_t dt = HAL_Time_SCR1TIM_Micros() - log_t0;
            log_t0 = HAL_Time_SCR1TIM_Micros();
            const uint32_t blk_per_s = (dt > 0U) ? (LOOP_LOG_EVERY * 1000000UL / dt) : 0U;
            xprintf("[LOOP] blk=%lu ~%lu blk/s adc=%u..%u pk=%u\r\n", (unsigned long)blocks,
                    (unsigned long)blk_per_s, (unsigned)lo, (unsigned)hi, (unsigned)pk_win_max);
            pk_win_max = 0U;
        }
    }
}

static HAL_StatusTypeDef record_shot(void) {
    tflm_reset();
    spectrogram_kiss_release_fft(&sk);
    adc_ram_capture_reset();
    adc_ram_capture_bind_fft(&sk);
    const unsigned max_blk = adc_ram_capture_max_blocks();

    if (adc_ram_capture_append_block(dma_buf) != HAL_OK) {
        return HAL_ERROR;
    }
    for (unsigned b = 1U; b < max_blk; b++) {
        if (dma_wait_block() != HAL_OK) {
            return HAL_TIMEOUT;
        }
        if (adc_ram_capture_append_block(dma_buf) != HAL_OK) {
            return HAL_ERROR;
        }
    }
    xprintf("[SHOT] stored %u blk\r\n", (unsigned)adc_ram_capture_block_count());
    return HAL_OK;
}

static void shot_stash_fft_blocks(unsigned n_blk) {
    shot_fft_stashed = false;
    for (unsigned b = 0U; b < n_blk; b++) {
        if (!adc_ram_capture_block_in_fft_mem(&sk, b)) {
            continue;
        }
        const uint16_t *pcm = adc_ram_capture_block_samples(b);
        if (pcm == NULL) {
            continue;
        }
        memcpy(dma_buf, pcm, DMA_SAMPLES * sizeof(uint16_t));
        shot_fft_stash_blk = (uint8_t)b;
        shot_fft_stashed = true;
    }
}

static const uint16_t *shot_pcm_block(unsigned block_index) {
    if (shot_fft_stashed && (unsigned)shot_fft_stash_blk == block_index) {
        return dma_buf;
    }
    return adc_ram_capture_block_samples(block_index);
}

static void process_shot(void) {
    const unsigned n_blk = adc_ram_capture_block_count();
    if (n_blk == 0U) {
        return;
    }

    shot_stash_fft_blocks(n_blk);
    if (!spectrogram_kiss_ensure_fft(&sk)) {
        xprintf("[SHOT] fft init failed\r\n");
        return;
    }

    spectrogram_kiss_reset_stream(&sk);
    uint16_t pk_max = 0U;
    const uint32_t t_fft0 = HAL_Time_SCR1TIM_Micros();

    for (unsigned b = 0U; b < n_blk; b++) {
        const uint16_t *pcm = shot_pcm_block(b);
        if (pcm == NULL) {
            xprintf("[SHOT] bad block %u\r\n", (unsigned)b);
            return;
        }
        memcpy(dma_buf, pcm, DMA_SAMPLES * sizeof(uint16_t));
        const uint16_t pk = block_peak(dma_buf, DMA_SAMPLES);
        if (pk > pk_max) {
            pk_max = pk;
        }
        if (b == 0U && n_blk > 1U) {
            spectrogram_kiss_push_adc_block(&sk, dma_buf, DMA_SAMPLES, NULL);
        } else {
            spectrogram_kiss_push_adc_block(&sk, dma_buf, DMA_SAMPLES, feat_live);
        }
    }

    const uint16_t hops = spectrogram_kiss_slice_count(&sk);
    const uint32_t fft_ms = (HAL_Time_SCR1TIM_Micros() - t_fft0) / 1000U;
    if (hops == 0U) {
        xprintf("[SHOT] no features\r\n");
        return;
    }

    const uint32_t t_init0 = HAL_Time_SCR1TIM_Micros();
    if (tflm_init() != 0) {
        xprintf("[SHOT] tflm_init err=%d\r\n", tflm_last_error());
        tflm_reset();
        return;
    }

    size_t in_sz = 0U;
    int8_t *in = (int8_t *)tflm_input_buffer(&in_sz);
    if (in == NULL || in_sz != (size_t)SK_FEATURE_COUNT) {
        xprintf("[SHOT] bad input %lu\r\n", (unsigned long)in_sz);
        tflm_reset();
        return;
    }

    spectrogram_kiss_pack_for_inference(&sk, feat_live, hops, in);
    const unsigned sat_bins = tensor_saturated_bins(in);
    const uint32_t init_ms = (HAL_Time_SCR1TIM_Micros() - t_init0) / 1000U;
    log_model_input(in);

    const uint32_t t_inv0 = HAL_Time_SCR1TIM_Micros();
    const int err = tflm_invoke();
    const uint32_t inv_ms = (HAL_Time_SCR1TIM_Micros() - t_inv0) / 1000U;

    if (err != 0) {
        xprintf("[SHOT] invoke err=%d\r\n", tflm_last_error());
    } else {
        const int cls = tflm_get_result();
        const unsigned speech0 =
            (SK_SPEECH_END_COL + 1U >= hops) ? (unsigned)(SK_SPEECH_END_COL + 1U - hops) : 0U;
        xprintf("[SHOT] hops=%u pk=%u sat=%u col%u..%u cls=%s fft=%lums init=%lums inv=%lums\r\n",
                (unsigned)hops, (unsigned)pk_max, sat_bins, speech0, (unsigned)SK_SPEECH_END_COL,
                cls_name(cls), (unsigned long)fft_ms, (unsigned long)init_ms, (unsigned long)inv_ms);
        tflm_log_output_scores();
    }
    tflm_reset();
}

static void log_model_input(const int8_t *tensor) {
    if (tensor == NULL) {
        return;
    }
    xprintf("[TENSOR] shape %u %u\r\n", (unsigned)SK_SLICES, (unsigned)SK_BINS);
    xprintf("[TENSOR] begin\r\n");
    for (unsigned col = 0U; col < SK_SLICES; col++) {
        xprintf("[TENSOR] col%02u:", col);
        for (unsigned bin = 0U; bin < SK_BINS; bin++) {
            xprintf(" %d", (int)tensor[(size_t)col * SK_BINS + bin]);
        }
        xprintf("\r\n");
    }
    xprintf("[TENSOR] end\r\n");
}

static void warm_noise_quiet(void) {
    for (unsigned b = 0U; b < 8U; b++) {
        if (dma_wait_block() != HAL_OK) {
            break;
        }
        spectrogram_kiss_push_adc_block(&sk, dma_buf, DMA_SAMPLES, NULL);
    }
}

static uint16_t quiet_cal(void) {
    uint32_t sum = 0U;
    unsigned n = 0U;
    for (unsigned b = 0U; b < QUIET_BLOCKS; b++) {
        if (dma_wait_block() != HAL_OK) {
            break;
        }
        for (unsigned i = 0U; i < DMA_SAMPLES; i++) {
            sum += dma_buf[i];
            n++;
        }
    }
    return (n > 0U) ? (uint16_t)(sum / n) : 2048U;
}

static unsigned tensor_saturated_bins(const int8_t *tensor) {
    unsigned sat = 0U;
    for (unsigned i = 0U; i < SK_FEATURE_COUNT; i++) {
        if (tensor[i] >= 127 || tensor[i] <= -128) {
            sat++;
        }
    }
    return sat;
}

static uint16_t block_peak(const uint16_t *s, unsigned n) {
    uint16_t lo = 0xFFFFU;
    uint16_t hi = 0U;
    for (unsigned i = 0U; i < n; i++) {
        if (s[i] < lo) {
            lo = s[i];
        }
        if (s[i] > hi) {
            hi = s[i];
        }
    }
    const uint16_t d_hi = (hi >= adc_mid) ? (hi - adc_mid) : (adc_mid - hi);
    const uint16_t d_lo = (lo >= adc_mid) ? (lo - adc_mid) : (adc_mid - lo);
    return (d_hi > d_lo) ? d_hi : d_lo;
}

static HAL_StatusTypeDef dma_wait_block(void) {
    const uint32_t len = (uint32_t)(DMA_SAMPLES * sizeof(uint16_t)) - 1U;
    dma_done = 0U;
    DMA_TIMER->INT_CLEAR = 0xFFFFFFFFu;
    HAL_DMA_Start(&hdma_ch, (void *)&ANALOG_REG->ADC_VALUE, dma_buf, len);

    uint32_t saw_busy = 0U;
    const uint32_t deadline = HAL_Time_SCR1TIM_Micros() +
                              ((DMA_SAMPLES * 1000000UL) / SAMPLE_RATE_HZ) * 3U;
    while (dma_done == 0U) {
        if (HAL_DMA_GetChannelReadyStatus(&hdma_ch) == 0) {
            saw_busy = 1U;
        } else if (saw_busy != 0U) {
            dma_done = 1U;
            break;
        }
        if (HAL_Time_SCR1TIM_Micros() >= deadline) {
            return HAL_TIMEOUT;
        }
    }
    return HAL_OK;
}

static uint32_t timer_hz(void) {
    return HAL_PCC_GetSysClockFreq() / ((PM->DIV_AHB + 1U) * (PM->DIV_APB_P + 1U));
}

static void timer_init(void) {
    const uint32_t hz = timer_hz();
    htimer.Instance = DMA_TIMER;
    htimer.Top = (hz / SAMPLE_RATE_HZ) - 1U;
    htimer.Clock.Prescaler = 0;
    htimer.Clock.Source = TIMER32_SOURCE_TIM1_HCLK;
    htimer.CountMode = TIMER32_COUNTMODE_FORWARD;
    htimer.State = TIMER32_STATE_ENABLE;
    htimer.InterruptMask = 0;
    HAL_Timer32_Init(&htimer);
    DMA_TIMER->INT_CLEAR = 0xFFFFFFFFu;
}

static void adc_init(void) {
    hadc.Instance = ANALOG_REG;
    hadc.Init.Sel = ADC_CHANNEL0;
    hadc.Init.EXTRef = ADC_EXTREF_OFF;
    hadc.Init.EXTClb = ADC_EXTCLB_CLBREF;
    HAL_ADC_Init(&hadc);
    HAL_ADC_ContinuousEnable(&hadc);
}

static void dma_ch_config(DMA_ChannelHandleTypeDef *ch) {
    ch->dma = &hdma;
    ch->ChannelInit.Channel = DMA_CHANNEL_0;
    ch->ChannelInit.Priority = DMA_CHANNEL_PRIORITY_HIGH;
    ch->ChannelInit.ReadMode = DMA_CHANNEL_MODE_PERIPHERY;
    ch->ChannelInit.ReadInc = DMA_CHANNEL_INC_DISABLE;
    ch->ChannelInit.ReadSize = DMA_CHANNEL_SIZE_HALFWORD;
    ch->ChannelInit.ReadBurstSize = 1U;
    ch->ChannelInit.ReadRequest = DMA_REQ;
    ch->ChannelInit.ReadAck = DMA_CHANNEL_ACK_ENABLE;
    ch->ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.WriteSize = DMA_CHANNEL_SIZE_HALFWORD;
    ch->ChannelInit.WriteBurstSize = 1U;
    ch->ChannelInit.WriteRequest = 0;
    ch->ChannelInit.WriteAck = DMA_CHANNEL_ACK_DISABLE;
}

static void dma_init(void) {
    hdma.Instance = DMA_CONFIG;
    hdma.CurrentValue = DMA_CURRENT_VALUE_ENABLE;
    HAL_DMA_Init(&hdma);
}

void SystemClock_Config(void) {
    PCC_InitTypeDef pcc = {0};
    pcc.OscillatorEnable = PCC_OSCILLATORTYPE_ALL;
    pcc.FreqMon.OscillatorSystem = PCC_OSCILLATORTYPE_OSC32M;
    pcc.FreqMon.ForceOscSys = PCC_FORCE_OSC_SYS_UNFIXED;
    pcc.FreqMon.Force32KClk = PCC_FREQ_MONITOR_SOURCE_OSC32K;
    pcc.AHBDivider = 0;
    pcc.APBMDivider = 0;
    pcc.APBPDivider = 0;
    pcc.HSI32MCalibrationValue = 128;
    pcc.LSI32KCalibrationValue = 8;
    pcc.RTCClockSelection = PCC_RTC_CLOCK_SOURCE_AUTO;
    pcc.RTCClockCPUSelection = PCC_CPU_RTC_CLOCK_SOURCE_OSC32K;
    HAL_PCC_Config(&pcc);
}

static void clock_init(void) {
    SystemClock_Config();
}

static void usart_init(void) {
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
