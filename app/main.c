#include "mik32_hal_adc.h"
#include "mik32_hal_usart.h"
#include "mik32_hal_scr1_timer.h"
#include "xprintf.h"

#define ADC_CHANNEL_TOTAL                6U
#define ADC_CONVERSIONS_PER_CHANNEL      2U
#define ADC_RAW_VALUE_MAX                4095U
#define ADC_REF_VOLTAGE_MV               1200U /* mV */
#define MILLIVOLTS_PER_VOLT              1000U
#define LOOP_DELAY_MS                    800U


static void SystemClock_Config();
static void USART_Init();

static USART_HandleTypeDef husart0;
ADC_HandleTypeDef hadc;

static void ADC_Init(void);

int main()
{
    uint16_t adc_value = 0U;
    uint32_t integer_part = 0U;
    uint32_t fractional_part = 0U;
    SystemClock_Config();

    USART_Init();
    ADC_Init();

    while (1)
    {
        HAL_USART_Print(&husart0, "YADRO RISCV\r\n", USART_TIMEOUT_DEFAULT);
        for (uint32_t channel = 0U; channel < ADC_CHANNEL_TOTAL; channel++) {
            for (uint32_t conversion = 0U; conversion < ADC_CONVERSIONS_PER_CHANNEL; conversion++) {
                HAL_ADC_SINGLE_AND_SET_CH(hadc.Instance, channel);
                adc_value = HAL_ADC_WaitAndGetValue(&hadc);
                if (0U == conversion) {
                    continue;
                }
                integer_part = adc_value * ADC_REF_VOLTAGE_MV / ADC_RAW_VALUE_MAX / MILLIVOLTS_PER_VOLT;
                fractional_part = adc_value * ADC_REF_VOLTAGE_MV / ADC_RAW_VALUE_MAX % MILLIVOLTS_PER_VOLT;
                xprintf("ADC[%d]: %04d/%d (%d,%03d V)\r\n", channel, adc_value, ADC_RAW_VALUE_MAX, integer_part, fractional_part);
            }
        }
        HAL_DelayMs(LOOP_DELAY_MS);
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

static void ADC_Init(void) {
    hadc.Instance = ANALOG_REG;
    hadc.Init.EXTRef = ADC_EXTREF_OFF;
    hadc.Init.EXTClb = ADC_EXTCLB_CLBREF;

    HAL_ADC_Init(&hadc);
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
