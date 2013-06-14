#include "stm32f4xx_conf.h"
#include "stm32f4_discovery.h"
#include "math.h"
#include "arm_math.h"
#include "cnc.h"

static volatile uint8_t adcValue[2] = {0, 0};

static const struct {
    GPIO_TypeDef *gpio;
    uint16_t xDirection, xStep, yDirection, yStep, zDirection, zStep;
} motorsPinout = {
        .gpio=GPIOE,
        .xDirection = GPIO_Pin_3,
        .xStep = GPIO_Pin_4,
        .yDirection = GPIO_Pin_5,
        .yStep=GPIO_Pin_6,
        .zDirection=GPIO_Pin_7,
        .zStep=GPIO_Pin_8};

static const struct {
    GPIO_TypeDef *gpio;
    uint16_t plugged, xControl, yControl;
} uiPinout = {
        .gpio = GPIOA,
        .xControl=GPIO_Pin_1,
        .yControl=GPIO_Pin_2};

volatile cnc_memory_t cncMemory = {
        .position = {.x=0, .y=0, .z=0},
        .parameters = {
                .stepsPerMillimeter=640,
                .maxSpeed = 3000,
                .maxAcceleration = 150,
                .clockFrequency = 200000},
        .state = READY,
        .lastEvent = {NULL_EVENT, 0, 0, 0},
        .running = 0};

static const struct {
    unsigned int x:1, y:1, z:1;
} motorDirection = {
        .x = 0,
        .y = 0,
        .z = 1};

static struct {
    float32_t x, y;
    float32_t deadzoneRadius;
    int32_t minFeed;
    int32_t maxFeed;
    uint8_t zeroX, zeroY;
    uint32_t steps;
    uint32_t calls;
    float32_t previousCoord, newCoord;
} manualControlStatus = {
        .x=0, .y=0,
        .deadzoneRadius = 0.1F,
        .minFeed = 30,
        .maxFeed = 3000,
        .zeroX = 128,
        .zeroY = 128};

step_t nextManualStep() {
    float32_t x = (adcValue[0] - manualControlStatus.zeroX) / 128.0F;
    float32_t y = (adcValue[1] - manualControlStatus.zeroY) / 128.0F;
    float32_t magnitude = hypotf(x, y);
    if (magnitude > 1)
        magnitude = 1;
    float32_t factor = (magnitude - manualControlStatus.deadzoneRadius) / ((1 - manualControlStatus.deadzoneRadius) * magnitude);
    if (factor <= 0)
        factor = 0;
    x *= factor;
    y *= factor;
    magnitude *= factor;
    manualControlStatus.calls++;
    if (x != manualControlStatus.x || y != manualControlStatus.y) {
        manualControlStatus.x = x;
        manualControlStatus.y = y;
        manualControlStatus.steps = 0;
    }
    float32_t minFeedSec = manualControlStatus.minFeed / 60.0F;
    float32_t maxFeedSec = manualControlStatus.maxFeed / 60.0F;
    float32_t feedAmplitude = maxFeedSec - minFeedSec;
    float32_t feedSec = feedAmplitude * magnitude;
    uint16_t duration = (uint16_t) (cncMemory.parameters.clockFrequency / feedSec / cncMemory.parameters.stepsPerMillimeter);
    step_t result = {
            .duration = magnitude ? duration : 0,
            .axes = {
                    .xDirection = (unsigned int) (x >= 0),
                    .yDirection = (unsigned int) (y >= 0),
                    .zDirection = 0,
                    .zStep = 0}};
    if (x || y) {
        manualControlStatus.steps++;
        if (fabs(x) > fabs(y)) {
            float32_t xsign = (x > 0 ? 1 : -1);
            float32_t slope = xsign * y / x;
            manualControlStatus.previousCoord = (manualControlStatus.steps - 1) * slope;
            manualControlStatus.newCoord = manualControlStatus.steps * slope;
            result.axes.xStep = 1;
            result.axes.yStep = (unsigned int) (roundf(manualControlStatus.newCoord) != roundf(manualControlStatus.previousCoord));
        } else {
            float32_t ysign = (y > 0 ? 1 : -1);
            float32_t slope = ysign * x / y;
            manualControlStatus.previousCoord = (manualControlStatus.steps - 1) * slope;
            manualControlStatus.newCoord = manualControlStatus.steps * slope;
            result.axes.xStep = (unsigned int) (roundf(manualControlStatus.newCoord) != roundf(manualControlStatus.previousCoord));
            result.axes.yStep = 1;
        }
    }
    return result;
}

static step_t nextProgramStep() {
    uint16_t nextDuration = 0;
    nextDuration |= readBuffer();
    nextDuration |= readBuffer() << 8;
    return (step_t) {
            .duration = nextDuration,
            .axes = ((union {
                axes_t s;
                uint8_t b;
            }) {.b = readBuffer()}).s};
}

static void executeStep(step_t step) {
    float32_t sqrt2 = sqrtf(2);
    float32_t sqrt3 = sqrtf(3);
    GPIO_ResetBits(motorsPinout.gpio, motorsPinout.xDirection | motorsPinout.xStep
            | motorsPinout.yDirection | motorsPinout.yStep
            | motorsPinout.zDirection | motorsPinout.zStep);
    cncMemory.currentStep = step;
    if (step.duration) {
        STM_EVAL_LEDOn(LED6);
        uint16_t duration = step.duration;
        int32_t axesCount = step.axes.xStep + step.axes.yStep + step.axes.zStep;
        if (axesCount == 2)
            duration *= sqrt2;
        if (axesCount == 3)
            duration *= sqrt3;
        TIM3->ARR = duration;
        TIM_SelectOnePulseMode(TIM3, TIM_OPMode_Single);
        TIM_Cmd(TIM3, ENABLE);
    } else {
        cncMemory.running = 0;
        TIM3->ARR = 10000;
        TIM_SelectOnePulseMode(TIM3, TIM_OPMode_Single);
        TIM_Cmd(TIM3, ENABLE);
    }
}

void executeNextStep() {
    cncMemory.running = 1;
    if (cncMemory.state == MANUAL_CONTROL)
        executeStep(nextManualStep());
    else if (cncMemory.state == RUNNING_PROGRAM)
        executeStep(nextProgramStep());
    else
        cncMemory.running = 0;
}

void updatePosition(step_t step) {
    if (step.axes.xStep)
        cncMemory.position.x += step.axes.xDirection ? 1 : -1;
    if (step.axes.yStep)
        cncMemory.position.y += step.axes.yDirection ? 1 : -1;
    if (step.axes.zStep)
        cncMemory.position.z += step.axes.zDirection ? 1 : -1;
}

__attribute__ ((used)) void TIM3_IRQHandler(void) {
    if (TIM_GetITStatus(TIM3, TIM_IT_CC1) != RESET) {
        TIM_ClearITPendingBit(TIM3, TIM_IT_CC1);
        uint16_t directions = 0;
        if (cncMemory.currentStep.axes.xDirection ^ motorDirection.x)
            directions |= motorsPinout.xDirection;
        if (cncMemory.currentStep.axes.yDirection ^ motorDirection.y)
            directions |= motorsPinout.yDirection;
        if (cncMemory.currentStep.axes.zDirection ^ motorDirection.z)
            directions |= motorsPinout.zDirection;
        GPIO_SetBits(motorsPinout.gpio, directions);
    }
    if (TIM_GetITStatus(TIM3, TIM_IT_CC2) != RESET) {
        TIM_ClearITPendingBit(TIM3, TIM_IT_CC2);
        uint16_t steps = 0;
        if (cncMemory.currentStep.axes.xStep)
            steps |= motorsPinout.xStep;
        if (cncMemory.currentStep.axes.yStep)
            steps |= motorsPinout.yStep;
        if (cncMemory.currentStep.axes.zStep)
            steps |= motorsPinout.zStep;
        GPIO_SetBits(motorsPinout.gpio, steps);
        updatePosition(cncMemory.currentStep);
    }
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
        STM_EVAL_LEDOff(LED6);
        executeNextStep();
    }
}

void zeroJoystick() {
    manualControlStatus.zeroX = adcValue[0];
    manualControlStatus.zeroY = adcValue[1];
}

int main(void) {
    //enable FPU
    SCB->CPACR |= 0b000000000111100000000000000000000UL;

    STM_EVAL_LEDInit(LED3);
    STM_EVAL_LEDInit(LED4);
    STM_EVAL_LEDInit(LED5);
    STM_EVAL_LEDInit(LED6);
    STM_EVAL_LEDOff(LED3);
    STM_EVAL_LEDOff(LED4);
    STM_EVAL_LEDOff(LED5);
    STM_EVAL_LEDOff(LED6);

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOE, ENABLE);

    GPIO_Init(motorsPinout.gpio, &(GPIO_InitTypeDef) {
            .GPIO_Pin = motorsPinout.xDirection | motorsPinout.xStep
                    | motorsPinout.yDirection | motorsPinout.yStep
                    | motorsPinout.zDirection | motorsPinout.zStep,
            .GPIO_Mode = GPIO_Mode_OUT,
            .GPIO_Speed = GPIO_Speed_2MHz,
            .GPIO_OType = GPIO_OType_PP,
            .GPIO_PuPd = GPIO_PuPd_NOPULL});
    GPIO_Init(uiPinout.gpio, &(GPIO_InitTypeDef) {
            .GPIO_Pin = uiPinout.xControl | uiPinout.yControl,
            .GPIO_Mode = GPIO_Mode_AN,
            .GPIO_PuPd = GPIO_PuPd_NOPULL});

    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    NVIC_Init(&(NVIC_InitTypeDef) {
            .NVIC_IRQChannel = TIM3_IRQn,
            .NVIC_IRQChannelPreemptionPriority = 0,
            .NVIC_IRQChannelSubPriority = 0,
            .NVIC_IRQChannelCmd = ENABLE});

    uint16_t PrescalerValue = (uint16_t) ((SystemCoreClock / 2) / cncMemory.parameters.clockFrequency) - 1;
    TIM_Cmd(TIM3, DISABLE);
    TIM_UpdateRequestConfig(TIM3, TIM_UpdateSource_Regular);
    TIM_SelectOnePulseMode(TIM3, TIM_OPMode_Single);
    TIM_SetCounter(TIM3, 10000);

    TIM_TimeBaseInit(TIM3, &((TIM_TimeBaseInitTypeDef) {
            .TIM_Period = 10000,
            .TIM_Prescaler = PrescalerValue,
            .TIM_ClockDivision = 0,
            .TIM_CounterMode = TIM_CounterMode_Down}));
    /* Channel1 for direction */
    TIM_OC1Init(TIM3, &(TIM_OCInitTypeDef) {
            .TIM_OCMode = TIM_OCMode_PWM1,
            .TIM_OutputState = TIM_OutputState_Enable,
            .TIM_Pulse = 2,
            .TIM_OCPolarity = TIM_OCPolarity_High});
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Disable);
    /* Channel2 for step */
    TIM_OC2Init(TIM3, &(TIM_OCInitTypeDef) {
            .TIM_OCMode = TIM_OCMode_PWM1,
            .TIM_OutputState = TIM_OutputState_Enable,
            .TIM_Pulse = 1,
            .TIM_OCPolarity = TIM_OCPolarity_High});

    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Disable);
    TIM_ITConfig(TIM3, TIM_IT_CC1| TIM_IT_CC2 | TIM_IT_Update, ENABLE);
    initUSB();

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_ADC1, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);
    ADC_CommonInit(&(ADC_CommonInitTypeDef) {
            .ADC_Mode = ADC_Mode_Independent,
            .ADC_Prescaler = ADC_Prescaler_Div2,
            .ADC_DMAAccessMode = ADC_DMAAccessMode_Disabled,
            .ADC_TwoSamplingDelay = ADC_TwoSamplingDelay_5Cycles});
    ADC_Init(ADC1, &(ADC_InitTypeDef) {
            .ADC_Resolution = ADC_Resolution_8b,
            .ADC_ScanConvMode = ENABLE,
            .ADC_ContinuousConvMode = ENABLE,
            .ADC_ExternalTrigConvEdge = ADC_ExternalTrigConvEdge_None,
            .ADC_DataAlign = ADC_DataAlign_Right,
            .ADC_NbrOfConversion = 2});
    DMA_Init(DMA2_Stream0, &(DMA_InitTypeDef) {
            .DMA_Channel = DMA_Channel_0,
            .DMA_PeripheralBaseAddr = (uint32_t) &(ADC1->DR),
            .DMA_Memory0BaseAddr = (uint32_t) adcValue,
            .DMA_DIR = DMA_DIR_PeripheralToMemory,
            .DMA_BufferSize = sizeof(adcValue),
            .DMA_PeripheralInc = DMA_PeripheralInc_Disable,
            .DMA_MemoryInc = DMA_MemoryInc_Enable,
            .DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte,
            .DMA_MemoryDataSize = DMA_MemoryDataSize_Byte,
            .DMA_Mode = DMA_Mode_Circular,
            .DMA_Priority = DMA_Priority_High,
            .DMA_FIFOMode = DMA_FIFOMode_Disable,
            .DMA_FIFOThreshold = DMA_FIFOThreshold_HalfFull,
            .DMA_MemoryBurst = DMA_MemoryBurst_Single,
            .DMA_PeripheralBurst = DMA_PeripheralBurst_Single});
    DMA_Cmd(DMA2_Stream0, ENABLE);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_1, 1, ADC_SampleTime_3Cycles);
    ADC_RegularChannelConfig(ADC1, ADC_Channel_2, 2, ADC_SampleTime_3Cycles);
    ADC_DMARequestAfterLastTransferCmd(ADC1, ENABLE);
    ADC_DMACmd(ADC1, ENABLE);
    ADC_Cmd(ADC1, ENABLE);
    ADC_SoftwareStartConv(ADC1);
    while (1);
}
