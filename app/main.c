
void *__dso_handle __attribute__((weak));

#include "dma_config.h"
#include "epic.h"
#include "mik32_hal_irq.h"
#include "mik32_hal_adc.h"
#include "mik32_hal_dma.h"
#include "mik32_hal_usart.h"
#include "mik32_hal_timer32.h"
#include "mik32_memory_map.h"
#include "power_manager.h"
#include "timer32.h"
#include "xprintf.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SYSTEM_FREQ_HZ 32000000UL
#define ADC_SAMPLE_RATE_HZ 16000U
#define ADC_SAMPLE_TIMER_TOP ((SYSTEM_FREQ_HZ / ADC_SAMPLE_RATE_HZ) - 1U)

#define ADC_DMA_HALFWORDS 32U

/*
 * Block diagram: TIMER32_0 sits on APB_M (with PM/EPIC); TIMER32_1/2 sit on APB_P with
 * SPI/I2C/UART. DMA "timer request" lines are wired from the APB_P timers — TIMER32_0
 * does not advance paced DMA (LEN stays at initial value; no grants). Use TIMER32_1.
 */
#define ADC_DMA_PACE_TIMER   TIMER32_1
#define ADC_DMA_PACE_REQUEST DMA_CHANNEL_TIMER32_1_REQUEST

/** Extra trace inside HAL_DMA_Wait inner loop (adds UART spam if set high). */
#define DMA_WAIT_TRACE_INTERVAL 200000U

static void SystemClock_Config(void);
static void USART_Init(void);
static void TMR_Init(void);
static void ADC_Init(void);
static void DMA_Init(void);
static void configure_adc_timer_dma(DMA_ChannelHandleTypeDef *ch);
static void configure_mem_dma(DMA_ChannelHandleTypeDef *ch);
static void dump_banner_hypotheses(void);
static void dump_clocks_and_bases(void);
static void dump_pace_timer(const char *tag);
static void dump_dma_global(const char *tag);
static void dump_dma_channel(unsigned ch_idx, const char *tag);
static HAL_StatusTypeDef dma_wait_trace(DMA_ChannelHandleTypeDef *ch, uint32_t max_iters,
                                        const char *ctx);
static HAL_StatusTypeDef dma_wait_busy_then_done(DMA_ChannelHandleTypeDef *ch, uint32_t max_iters,
                                                 const char *ctx);
static void dma_adc_prepare_for_start(void);
static int dma_mem_self_test(void);

static USART_HandleTypeDef husart0;
static ADC_HandleTypeDef hadc;
static DMA_InitTypeDef hdma;
static TIMER32_HandleTypeDef htimer;

static uint16_t adc_buffer[ADC_DMA_HALFWORDS];
static DMA_ChannelHandleTypeDef hdma_ch_adc;
static DMA_ChannelHandleTypeDef hdma_ch_mem;

static uint8_t mem_src[64];
static uint8_t mem_dst[64];

int main(void) {
    SystemClock_Config();
    USART_Init();

    xprintf("\r\n======== ADC+TIMER+DMA BRING-UP (no ML) ========\r\n");
    dump_banner_hypotheses();
    dump_clocks_and_bases();

    ADC_Init();
    DMA_Init();

    xprintf("[BOOT] dma_mem_self_test (no timer, no ACK) ...\r\n");
    if (dma_mem_self_test() != 0) {
        xprintf("[BOOT] ABORT: DMA mem-mem failed; fix clocks/DMA before ADC path.\r\n");
        while (1) {
        }
    }
    xprintf("[BOOT] mem-mem OK\r\n");

    TMR_Init();
    dump_pace_timer("after TMR_Init");

    configure_adc_timer_dma(&hdma_ch_adc);
    dump_dma_global("after configure_adc_timer_dma");

    __HAL_PCC_EPIC_CLK_ENABLE();
    EPIC->CLEAR = (1u << EPIC_LINE_DMA_S);
    HAL_DMA_ClearIrq(&hdma);
    xprintf("[BOOT] EPIC RAW after CLEAR dma line: 0x%08lx\r\n", (unsigned long)HAL_EPIC_GetRawStatus());

    HAL_Timer32_Start(&htimer);
    dump_pace_timer("after HAL_Timer32_Start");

    HAL_IRQ_EnableInterrupts();

    /* Prove pace timer counter moves (independent of DMA). */
    for (unsigned n = 0; n < 3U; n++) {
        uint32_t a = ADC_DMA_PACE_TIMER->VALUE;
        for (volatile uint32_t d = 0; d < 80000U; d++) {
        }
        uint32_t b = ADC_DMA_PACE_TIMER->VALUE;
        xprintf("[BOOT] TIMER32_1 VALUE sample%u: a=%lu b=%lu (expect a!=b if counting)\r\n", n,
                (unsigned long)a, (unsigned long)b);
    }

    uint32_t attempt = 0;
    while (1) {
        const uint32_t dma_len_bytes =
            (uint32_t)(ADC_DMA_HALFWORDS * sizeof(uint16_t)) - 1U;

        xprintf("\r\n---- attempt %lu ----\r\n", (unsigned long)attempt++);
        dump_pace_timer("top of loop");
        dump_dma_channel((unsigned)hdma_ch_adc.ChannelInit.Channel, "before HAL_DMA_Start");

        dma_adc_prepare_for_start();
        HAL_DMA_Start(&hdma_ch_adc, (void *)&ANALOG_REG->ADC_VALUE, adc_buffer, dma_len_bytes);

        dump_dma_channel((unsigned)hdma_ch_adc.ChannelInit.Channel, "after HAL_DMA_Start");

        HAL_StatusTypeDef w =
            dma_wait_busy_then_done(&hdma_ch_adc, DMA_TIMEOUT_DEFAULT, "adc+paced");
        if (w != HAL_OK) {
            xprintf("[FAIL] dma_wait returned %d — dumping post-timeout state\r\n", (int)w);
            dump_pace_timer("post-timeout");
            dump_dma_global("post-timeout");
            dump_dma_channel((unsigned)hdma_ch_adc.ChannelInit.Channel, "post-timeout");
            xprintf("[HINT] If TIMER VALUE never changed earlier: prescaler/HCLK/timer enable.\r\n");
            xprintf("[HINT] If mem-mem OK but LEN stuck: wrong DMA timer line (use TIMER32_1 REQ).\r\n");
            xprintf("[HINT] If CH.LEN stuck at full count: no DMA beats (no timer DMA events).\r\n");
            xprintf("ADC direct: %d.\r\n", (unsigned)ANALOG_REG->ADC_VALUE);
            continue;
        }

        xprintf("[OK] block done. adc[0]=%u adc[last]=%u\r\n", (unsigned)adc_buffer[0],
                (unsigned)adc_buffer[ADC_DMA_HALFWORDS - 1]);

        for (volatile uint32_t d = 0; d < 200000U; d++) {
        }
    }
}

static void dump_banner_hypotheses(void) {
    xprintf(
        "[INFO] Hypotheses we log for:\r\n"
        "  - DMA AHB clock off (PM CLK_AHB)\r\n"
        "  - TIMER32_1 APB_P clock off (DMA pacer must be TIMER32_1/2, not TIMER32_0)\r\n"
        "  - TIMER32_0 on APB_M does not drive DMA requests on this die\r\n"
        "  - Timer PRESCALER tap disabled (bit8 PRESCALER_ENABLE)\r\n"
        "  - Timer not enabled or TOP=0\r\n"
        "  - Wrong ANALOG_REG->ADC_VALUE address / bus\r\n"
        "  - DMA request mux wrong (must match timer instance, e.g. TIMER32_1 -> req 8)\r\n"
        "  - ReadAck pacing: no timer overflow => no DMA transfers\r\n"
        "  - CONFIG_STATUS READY polarity misunderstood\r\n"
        "  - LEN+1 not multiple of transfer width / burst packet (dma_docs 3.5.5, Table 9)\r\n"
        "  - EPIC/DMA irq masks irrelevant for HAL_DMA_Wait polling\r\n"
        "  - TIMER32 INT_FLAGS overflow stuck -> no new edges for DMA pacer\r\n"
        "  - Stale READY=1: HAL_DMA_Wait exits immediately (fake OK); need busy-then-done\r\n");
}

static void dump_clocks_and_bases(void) {
    xprintf("[MAP] RAM        0x%08lx  DMA_CONFIG 0x%08lx\r\n", (unsigned long)RAM_BASE_ADDRESS,
            (unsigned long)DMA_CONFIG_BASE_ADDRESS);
    xprintf("[MAP] TIMER32_1 (pacer) 0x%08lx  ANALOG_REG 0x%08lx\r\n",
            (unsigned long)TIMER32_1_BASE_ADDRESS, (unsigned long)ANALOG_REG_BASE_ADDRESS);
    xprintf("[MAP] adc_buffer=0x%08lx (SRAM)\r\n", (unsigned long)(uintptr_t)adc_buffer);

    xprintf("[CLK] PM CLK_AHB_SET snapshot: 0x%08lx (DMA bit expected)\r\n",
            (unsigned long)PM->CLK_AHB_SET);
    xprintf("[CLK] PM CLK_APB_M_SET snapshot: 0x%08lx\r\n", (unsigned long)PM->CLK_APB_M_SET);
    xprintf("[CLK] PM CLK_APB_P_SET snapshot: 0x%08lx (TIMER32_1 bit expected after init)\r\n",
            (unsigned long)PM->CLK_APB_P_SET);
}

static void dump_pace_timer(const char *tag) {
    xprintf("[TIM] %-14s EN=0x%08lx TOP=%lu VAL=%lu PSC=0x%08lx CTL=0x%08lx INT_FL=0x%08lx\r\n", tag,
            (unsigned long)ADC_DMA_PACE_TIMER->ENABLE, (unsigned long)ADC_DMA_PACE_TIMER->TOP,
            (unsigned long)ADC_DMA_PACE_TIMER->VALUE, (unsigned long)ADC_DMA_PACE_TIMER->PRESCALER,
            (unsigned long)ADC_DMA_PACE_TIMER->CONTROL, (unsigned long)ADC_DMA_PACE_TIMER->INT_FLAGS);
}

static void dump_dma_global(const char *tag) {
    xprintf("[DMA] %-14s CONFIG_STATUS=0x%08lx\r\n", tag,
            (unsigned long)DMA_CONFIG->CONFIG_STATUS);
}

static void dump_dma_ch_status_bits(unsigned ch_idx) {
    const uint32_t st = DMA_CONFIG->CONFIG_STATUS;
    const unsigned rdy = (unsigned)((st >> (DMA_STATUS_READY_S + ch_idx)) & 1u);
    const unsigned irq = (unsigned)((st >> (DMA_STATUS_CHANNEL_IRQ_S + ch_idx)) & 1u);
    const unsigned be =
        (unsigned)((st >> (DMA_STATUS_CHANNEL_BUS_ERROR_S + ch_idx)) & 1u);
    xprintf("      STATUS bits ch%u: READY=%u IRQ=%u BUS_ERR=%u\r\n", ch_idx, rdy, irq, be);
}

static void dump_dma_channel(unsigned ch_idx, const char *tag) {
    if (ch_idx >= DMA_CHANNEL_COUNT) {
        return;
    }
    const DMA_CHANNEL_TypeDef *c = &DMA_CONFIG->CHANNELS[ch_idx];
    xprintf("[DCH] %-14s ch%u SRC=0x%08lx DST=0x%08lx LEN=0x%08lx CFG=0x%08lx\r\n", tag, ch_idx,
            (unsigned long)c->SRC, (unsigned long)c->DST, (unsigned long)c->LEN,
            (unsigned long)c->CFG);
    uint32_t cfg = c->CFG;
    xprintf("      decode CFG: EN=%u RD_REQ=%lu WR_REQ=%lu RD_ACK=%u WR_ACK=%u "
            "RD_sz=%lu WR_sz=%lu\r\n",
            (unsigned)(cfg & DMA_CH_CFG_ENABLE_M ? 1U : 0U),
            (unsigned long)((cfg >> DMA_CH_CFG_READ_REQUEST_S) & 0xFu),
            (unsigned long)((cfg >> DMA_CH_CFG_WRITE_REQUEST_S) & 0xFu),
            (unsigned)(cfg & DMA_CH_CFG_READ_ACK_EN_M ? 1U : 0U),
            (unsigned)(cfg & DMA_CH_CFG_WRITE_ACK_EN_M ? 1U : 0U),
            (unsigned long)((cfg >> DMA_CH_CFG_READ_SIZE_S) & 3u),
            (unsigned long)((cfg >> DMA_CH_CFG_WRITE_SIZE_S) & 3u));
    dump_dma_ch_status_bits(ch_idx);
}

/**
 * Paced transfers: after HAL_DMA_Start the channel must go busy (READY clears) then
 * finish (READY sets). If we only wait for READY=1, a stale post-reset READY=1 yields
 * a false "OK in 1 spin" with no data moved.
 */
static HAL_StatusTypeDef dma_wait_busy_then_done(DMA_ChannelHandleTypeDef *ch, uint32_t max_iters,
                                                 const char *ctx) {
    const uint32_t ch_i = (uint32_t)ch->ChannelInit.Channel;
    const uint32_t mask = (1u << ch_i) << DMA_STATUS_READY_S;

    /* Phase A: wait for NOT ready == busy (armed). */
    uint32_t a = max_iters / 4U;
    if (a == 0U) {
        a = 1000U;
    }
    uint32_t spun = 0U;
    while (a-- != 0U) {
        spun++;
        uint32_t st = ch->dma->Instance->CONFIG_STATUS;
        if ((st & mask) == 0U) {
            xprintf("[WAIT] %s armed after %lu polls STATUS=0x%08lx\r\n", ctx,
                    (unsigned long)spun, (unsigned long)st);
            goto wait_done;
        }
    }
    xprintf("[WARN] %s never saw READY clear (still idle?) after %lu polls — bogus start?\r\n", ctx,
            (unsigned long)spun);

wait_done:
    return dma_wait_trace(ch, max_iters, ctx);
}

static void dma_adc_prepare_for_start(void) {
    /* Sticky overflow can starve DMA handshake of clean timer edges (seen INT_FL=0x1). */
    ADC_DMA_PACE_TIMER->INT_CLEAR = 0xFFFFFFFFu;

    HAL_DMA_ChannelDisable(&hdma_ch_adc);
    for (volatile uint32_t d = 0; d < 2000U; d++) {
    }
    HAL_DMA_ClearLocalIrq(&hdma);
}

static HAL_StatusTypeDef dma_wait_trace(DMA_ChannelHandleTypeDef *ch, uint32_t max_iters,
                                        const char *ctx) {
    const uint32_t ch_i = (uint32_t)ch->ChannelInit.Channel;
    const uint32_t mask = (1u << ch_i) << DMA_STATUS_READY_S;

    uint32_t iter = max_iters;
    uint32_t spun = 0U;

    while (iter-- != 0U) {
        spun++;
        uint32_t st = ch->dma->Instance->CONFIG_STATUS;
        if ((st & mask) != 0U) {
            xprintf("[WAIT] %s OK spun=%lu STATUS=0x%08lx\r\n", ctx, (unsigned long)spun,
                    (unsigned long)st);
            return HAL_OK;
        }
        if ((spun % DMA_WAIT_TRACE_INTERVAL) == 0U) {
            xprintf("[WAIT] %s still busy spun=%lu STATUS=0x%08lx CH.LEN=0x%08lx T32.VAL=%lu\r\n",
                    ctx, (unsigned long)spun, (unsigned long)st,
                    (unsigned long)DMA_CONFIG->CHANNELS[ch_i].LEN, (unsigned long)ADC_DMA_PACE_TIMER->VALUE);
        }
    }

    xprintf("[WAIT] %s TIMEOUT mask=0x%08lx last_STATUS=0x%08lx\r\n", ctx, (unsigned long)mask,
            (unsigned long)ch->dma->Instance->CONFIG_STATUS);
    return HAL_TIMEOUT;
}

static void configure_mem_dma(DMA_ChannelHandleTypeDef *ch) {
    ch->dma = &hdma;
    ch->ChannelInit.Channel = DMA_CHANNEL_0;
    ch->ChannelInit.Priority = DMA_CHANNEL_PRIORITY_MEDIUM;
    ch->ChannelInit.ReadMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.ReadInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.ReadSize = DMA_CHANNEL_SIZE_BYTE;
    ch->ChannelInit.ReadBurstSize = 0;
    ch->ChannelInit.ReadRequest = 0;
    ch->ChannelInit.ReadAck = DMA_CHANNEL_ACK_DISABLE;
    ch->ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.WriteSize = DMA_CHANNEL_SIZE_BYTE;
    ch->ChannelInit.WriteBurstSize = 0;
    ch->ChannelInit.WriteRequest = 0;
    ch->ChannelInit.WriteAck = DMA_CHANNEL_ACK_DISABLE;
}

static int dma_mem_self_test(void) {
    for (unsigned i = 0; i < sizeof(mem_src); i++) {
        mem_src[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
    }
    memset(mem_dst, 0, sizeof(mem_dst));

    configure_mem_dma(&hdma_ch_mem);
    HAL_DMA_Start(&hdma_ch_mem, mem_src, mem_dst, sizeof(mem_dst) - 1U);
    HAL_StatusTypeDef st = dma_wait_trace(&hdma_ch_mem, DMA_TIMEOUT_DEFAULT, "mem-mem");
    if (st != HAL_OK) {
        xprintf("[SELFTEST] mem-mem dma_wait=%d\r\n", (int)st);
        dump_dma_channel(0, "mem fail");
        return -1;
    }
    if (memcmp(mem_src, mem_dst, sizeof(mem_dst)) != 0) {
        xprintf("[SELFTEST] data mismatch after DMA\r\n");
        return -2;
    }
    return 0;
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

static void TMR_Init(void) {
    htimer.Instance = ADC_DMA_PACE_TIMER;
    htimer.Top = ADC_SAMPLE_TIMER_TOP;
    htimer.Clock.Prescaler = 0;
    htimer.Clock.Source = TIMER32_SOURCE_PRESCALER;
    htimer.CountMode = TIMER32_COUNTMODE_FORWARD;
    htimer.State = TIMER32_STATE_ENABLE;
    htimer.InterruptMask = 0;

    HAL_Timer32_Init(&htimer);

    /* timer32_docs §3.9.4: recommend INT_CLEAR = 0xFFFFFFFF before TIM_EN (HAL init uses 0x3FF). */
    ADC_DMA_PACE_TIMER->INT_CLEAR = 0xFFFFFFFFu;

    /*
     * HAL_Timer32_Prescaler_Set only writes the divide field; many MCUs also need an
     * explicit "prescaler enable / output enable" bit for the timer tick to run.
     * If this bit was clear, the pace timer VALUE would never change and paced DMA would
     * wait forever (your symptom).
     */
    {
        uint32_t psc_rd = ADC_DMA_PACE_TIMER->PRESCALER;
        uint32_t psc_wr = (htimer.Clock.Prescaler & 0xFFu) | TIMER32_PRESCALER_ENABLE_M;
        ADC_DMA_PACE_TIMER->PRESCALER = psc_wr;
        xprintf("[TMR] PRESCALER before=0x%08lx after=0x%08lx (ENABLE bit8 forced)\r\n",
                (unsigned long)psc_rd, (unsigned long)ADC_DMA_PACE_TIMER->PRESCALER);
    }
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

    ch->ChannelInit.Channel = DMA_CHANNEL_1;
    ch->ChannelInit.Priority = DMA_CHANNEL_PRIORITY_HIGH;

    ch->ChannelInit.ReadMode = DMA_CHANNEL_MODE_PERIPHERY;
    ch->ChannelInit.ReadInc = DMA_CHANNEL_INC_DISABLE;
    ch->ChannelInit.ReadSize = DMA_CHANNEL_SIZE_HALFWORD;
    /* dma_docs §3.5.5 / Table 9: 2^READ_BURST bytes must be multiple of read width.
     * Halfword = 2 B → need ≥2 B/packet (burst 1). Burst 0 → 1 B is invalid; channel can hang. */
    ch->ChannelInit.ReadBurstSize = 1U;
    ch->ChannelInit.ReadRequest = ADC_DMA_PACE_REQUEST;
    ch->ChannelInit.ReadAck = DMA_CHANNEL_ACK_ENABLE;

    ch->ChannelInit.WriteMode = DMA_CHANNEL_MODE_MEMORY;
    ch->ChannelInit.WriteInc = DMA_CHANNEL_INC_ENABLE;
    ch->ChannelInit.WriteSize = DMA_CHANNEL_SIZE_HALFWORD;
    ch->ChannelInit.WriteBurstSize = 1U;
    ch->ChannelInit.WriteRequest = 0;
    ch->ChannelInit.WriteAck = DMA_CHANNEL_ACK_DISABLE;

    xprintf("[CFG] ADC DMA ch=%d PACE_REQ=%d rd_burst=%uB wr_burst=%uB ReadAck=%d xfer=%uB LEN=%u\r\n",
            (int)ch->ChannelInit.Channel, (int)ADC_DMA_PACE_REQUEST,
            (unsigned)(1u << ch->ChannelInit.ReadBurstSize), (unsigned)(1u << ch->ChannelInit.WriteBurstSize),
            (int)DMA_CHANNEL_ACK_ENABLE,
            (unsigned)(ADC_DMA_HALFWORDS * sizeof(uint16_t)),
            (unsigned)((ADC_DMA_HALFWORDS * sizeof(uint16_t)) - 1U));
}

static void DMA_Init(void) {
    hdma.Instance = DMA_CONFIG;
    /*
     * dma_docs Table 10 bit CURRENT_VALUE: 0 = read live CHx_LEN/addr; 1 = frozen setup copy.
     * HAL DMA_CURRENT_VALUE_ENABLE = 0 → live (see 3.5.9). Needed to see LEN count down.
     */
    hdma.CurrentValue = DMA_CURRENT_VALUE_ENABLE;
    HAL_DMA_Init(&hdma);
    xprintf("[DMA] CurrentValue=ENABLE (live CHx_LEN, dma_docs 3.5.9)\r\n");
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
