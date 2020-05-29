/*
 * This file is part of Cleanflight and Betaflight.
 *
 * Cleanflight and Betaflight are free software. You can redistribute
 * this software and/or modify this software under the terms of the
 * GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * Cleanflight and Betaflight are distributed in the hope that they
 * will be useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software.
 *
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#if defined(USE_FREQ_SENSOR)

#include "build/build_config.h"
#include "build/debug.h"

#include "common/utils.h"

#include "drivers/io.h"
#include "drivers/nvic.h"
#include "drivers/time.h"
#include "drivers/timer.h"
#include "drivers/dshot.h"
#include "drivers/freq.h"

#include "pg/freq.h"


// Accepted frequence range
#define FREQ_RANGE_MIN        10.0
#define FREQ_RANGE_MAX        5000.0

// Prescaler limits
#define FREQ_PRESCALER_MIN    0x0001
#define FREQ_PRESCALER_MAX    0x2000

// Prescaler shift points
#define FREQ_SHIFT_MIN        0x1000
#define FREQ_SHIFT_MAX        0x4000

// Period init value
#define FREQ_PERIOD_INIT      0x2000

// Filtering coefficients
#define FREQ_PERIOD_COEFF     32
#define FREQ_FILTER_COEFF     8

// Maximum number of overflows
#define FREQ_MAX_MISSING      4

// Input signal max deviation from average 75%..150%
#define FREQ_PERIOD_MIN(p)    ((p)*3/4)
#define FREQ_PERIOD_MAX(p)    ((p)*3/2)


#define FILTER_UPDATE(_var,_value,_coef) \
    ((_var) += ((_value)-(_var))/(_coef))

#define UPDATE_FREQ_FILTER(_input,_freq) \
    FILTER_UPDATE((_input)->freq, _freq, FREQ_FILTER_COEFF)

#define UPDATE_PERIOD_FILTER(_input,_per) \
    FILTER_UPDATE((_input)->period, _per, FREQ_PERIOD_COEFF)

bool freqTimerInitialized = false;

typedef struct {

    float freq;
    float clock;

    int32_t  period;
    uint16_t capture;
    uint16_t missing;
    uint16_t prescaler;
    uint16_t overflows;

    timerCCHandlerRec_t edgeCb;
    timerOvrHandlerRec_t overflowCb;
    
    const timerHardware_t *timerHardware;

} freqInputPort_t;

static freqInputPort_t freqInputPorts[FREQ_SENSOR_PORT_COUNT];


static void freqSetBaseClock(freqInputPort_t *input, uint32_t prescaler)
{
    TIM_TypeDef *tim = input->timerHardware->tim;

    input->prescaler = prescaler;
    input->capture = 0;
    input->clock = (float)timerClock(input->timerHardware->tim) / prescaler;
    
    tim->PSC = prescaler - 1;
    tim->EGR = TIM_EGR_UG;
}

static void freqReset(freqInputPort_t *input)
{
    input->freq = 0.0f;
    input->period = FREQ_PERIOD_INIT;
    input->missing = 0;
    input->overflows = 0;
    
    freqSetBaseClock(input, FREQ_PRESCALER_MAX);
    
    DEBUG_SET(DEBUG_FREQSENSOR, 0, input->period);
    DEBUG_SET(DEBUG_FREQSENSOR, 1, 0);
#ifdef USE_DSHOT_TELEMETRY
    DEBUG_SET(DEBUG_FREQSENSOR, 2, getDshotTelemetry(0));
#endif
    //DEBUG_SET(DEBUG_FREQSENSOR, 2, 32 - __builtin_clz(input->prescaler));
    DEBUG_SET(DEBUG_FREQSENSOR, 3, lrintf(input->freq));
}

static void freqInputUpdate(freqInputPort_t *input, uint16_t period)
{
    UPDATE_PERIOD_FILTER(input, period);

    // Filtered period out of range. Change prescaler.
    if (input->period < FREQ_SHIFT_MIN && input->prescaler > FREQ_PRESCALER_MIN) {
        freqSetBaseClock(input, input->prescaler >> 1);
        input->period <<= 1;
        period <<= 1;
    }
    else if (input->period > FREQ_SHIFT_MAX && input->prescaler < FREQ_PRESCALER_MAX) {
        freqSetBaseClock(input, input->prescaler << 1);
        input->period >>= 1;
        period >>= 1;
    }

    // Signal conditioning. Update freq filter only if period within acceptable range.
    if (period > FREQ_PERIOD_MIN(input->period) && period < FREQ_PERIOD_MAX(input->period)) {
        float freq = input->clock / period;
        if (freq > FREQ_RANGE_MIN && freq < FREQ_RANGE_MAX) {
            UPDATE_FREQ_FILTER(input, freq);
        }
    }
    
    DEBUG_SET(DEBUG_FREQSENSOR, 0, input->period);
    DEBUG_SET(DEBUG_FREQSENSOR, 1, period);
#ifdef USE_DSHOT_TELEMETRY
    DEBUG_SET(DEBUG_FREQSENSOR, 2, getDshotTelemetry(0));
#endif
    //DEBUG_SET(DEBUG_FREQSENSOR, 2, 32 - __builtin_clz(input->prescaler));
    DEBUG_SET(DEBUG_FREQSENSOR, 3, lrintf(input->freq));
}

static void freqEdgeCallback(timerCCHandlerRec_t *cbRec, captureCompare_t capture)
{
    freqInputPort_t *input = container_of(cbRec, freqInputPort_t, edgeCb);

    if (input->capture) {
        freqInputUpdate(input, (capture - input->capture));
    }
    
    input->capture = capture;
    input->overflows = 0;
    input->missing = 0;
}

static void freqOverflowCallback(timerOvrHandlerRec_t *cbRec, captureCompare_t capture)
{
    UNUSED(capture);
    freqInputPort_t *input = container_of(cbRec, freqInputPort_t, overflowCb);

    input->overflows++;

    // Two overflows means no signal for a whole period
    if (input->overflows > 1) {
        input->missing++;
        // Reset after too many dead periods
        if (input->missing > FREQ_MAX_MISSING) {
            freqReset(input);
        }
        input->overflows = 0;
        input->capture = 0;
    }
}

#if defined(USE_HAL_DRIVER)
void freqICConfig(const timerHardware_t *timer, bool rising, uint16_t filter)
{
    TIM_HandleTypeDef *handle = timerFindTimerHandle(timer->tim);
    if (handle == NULL)
        return;

    TIM_IC_InitTypeDef sInitStructure;
    memset(&sInitStructure, 0, sizeof(sInitStructure));
    sInitStructure.ICPolarity = rising ? TIM_ICPOLARITY_RISING : TIM_ICPOLARITY_FALLING;
    sInitStructure.ICSelection = TIM_ICSELECTION_DIRECTTI;
    sInitStructure.ICPrescaler = TIM_ICPSC_DIV1;
    sInitStructure.ICFilter = filter;
    HAL_TIM_IC_ConfigChannel(handle, &sInitStructure, timer->channel);
    HAL_TIM_IC_Start_IT(handle, timer->channel);

}
#else
// TODO
#endif

void freqInit(const freqConfig_t *freqConfig)
{
    for (int port = 0; port < FREQ_SENSOR_PORT_COUNT; port++) {
        freqInputPort_t *input = &freqInputPorts[port];
        const timerHardware_t *timer = timerAllocate(freqConfig->ioTag[port], OWNER_FREQ, RESOURCE_INDEX(port));
        if (timer) {
            input->timerHardware = timer;
            input->freq = 0.0f;
            input->period = FREQ_PERIOD_INIT;
    
            IO_t io = IOGetByTag(freqConfig->ioTag[port]);
            IOInit(io, OWNER_FREQ, RESOURCE_INDEX(port));
            IOConfigGPIOAF(io, IOCFG_AF_PP_PD, timer->alternateFunction);

            configTimeBase(timer->tim, 0, timerClock(timer->tim));
            timerNVICConfigure(timerInputIrq(timer->tim));
            
            timerChCCHandlerInit(&input->edgeCb, freqEdgeCallback);
            timerChOvrHandlerInit(&input->overflowCb, freqOverflowCallback);
            timerChConfigCallbacks(timer, &input->edgeCb, &input->overflowCb);
            
            freqICConfig(timer, true, 4);
            freqReset(input);

            freqTimerInitialized = true;
        }
    }
}

float freqRead(uint8_t port)
{
    if (port < FREQ_SENSOR_PORT_COUNT)
        return freqInputPorts[port].freq;
    
    return 0.0f;
}

uint16_t freqGetERPM(uint8_t port)
{
    // HF3D TODO:  Most things that read this value are requesting it based on Motor number, which may or may not coincide with the RPM sensor.
    //   May not be an issue for most helis since they will only have one main motor and either an RPM sensor on the ESC/motor leads, or on the main shaft.
    
    if (port < FREQ_SENSOR_PORT_COUNT) {
        // Return eRPM/100 as expected by RPM filter, msp, etc.
        return (uint16_t) (freqInputPorts[port].freq * 60.0f / 100.0f);
    }
    
    return 0;    
    
}

bool isFreqSensorInitialized(void)
{
    return freqTimerInitialized;
}

#endif
