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
#define QUIET_BLOCKS 20U
// Params below should be changed according to your gain on mic
#define TRIGGER_PK 1000U
#define MIC_GAIN_Q8 8

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
static uint16_t adc_mid = 1630U;  // Found imprically for MAX9814
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
static HAL_StatusTypeDef record_shot(void);
static void shot_stash_fft_blocks(unsigned n_blk);
static const uint16_t *shot_pcm_block(unsigned block_index);
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

    adc_mid = quiet_cal();
    spectrogram_kiss_set_cal(&sk, adc_mid, MIC_GAIN_Q8);
    warm_noise_quiet();
    spectrogram_kiss_reset_stream(&sk);
    adc_ram_capture_reset();

    xprintf("[BOOT] mid=%u gain=%u pk>=%u cap=%uB\r\n", (unsigned)adc_mid, (unsigned)MIC_GAIN_Q8,
            (unsigned)TRIGGER_PK, (unsigned)adc_ram_capture_pool_bytes());

    for (;;) {
        if (dma_wait_block() != HAL_OK) {
            continue;
        }

        const uint16_t pk = block_peak(dma_buf, DMA_SAMPLES);
        if (pk >= TRIGGER_PK) {
            xprintf("[SHOOT] start to capture voice, pk = %d\n\r", pk);
            static uint32_t cooldown;
            if (cooldown > 0U) {
                cooldown--;
                continue;
            }
            if (record_shot() == HAL_OK) {
                process_shot();
            } else {
                xprintf("[SHOT] record fail\r\n");
            }
            cooldown = 80U;
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
        xprintf("[SHOT] block %d read\n\r", b);
        if (adc_ram_capture_append_block(dma_buf) != HAL_OK) {
            return HAL_ERROR;
        }
    }
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

    for (unsigned b = 0U; b < n_blk; b++) {
        const uint16_t *pcm = shot_pcm_block(b);
        if (pcm == NULL) {
            xprintf("[SHOT] bad blk %u\r\n", (unsigned)b);
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
    if (hops == 0U) {
        xprintf("[SHOT] no features\r\n");
        return;
    }

    if (tflm_init() != 0) {
        xprintf("[SHOT] init err=%d\r\n", tflm_last_error());
        tflm_reset();
        return;
    }

    size_t in_sz = 0U;
    int8_t *in = (int8_t *)tflm_input_buffer(&in_sz);
    if (in == NULL || in_sz != (size_t)SK_FEATURE_COUNT) {
        xprintf("[SHOT] bad input\r\n");
        tflm_reset();
        return;
    }

    spectrogram_kiss_pack_for_inference(&sk, feat_live, hops, in);

    if (tflm_invoke() != 0) {
        xprintf("[SHOT] invoke err=%d\r\n", tflm_last_error());
    } else {
        const int cls = tflm_get_result();
        xprintf("[SHOT] blk=%u hops=%u pk=%u cls=%s\r\n", (unsigned)n_blk, (unsigned)hops,
                (unsigned)pk_max, cls_name(cls));
    }
    tflm_reset();
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
        xprintf("[BOOT] taking silence blocks, %d/%d\n\r", b, QUIET_BLOCKS);
        for (unsigned i = 0U; i < DMA_SAMPLES; i++) {
            sum += dma_buf[i];
            n++;
        }
    }
    return (n > 0U) ? (uint16_t)(sum / n) : 2048U;
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
