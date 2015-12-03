/**
 ******************************************************************************
 * @file    pwm_hal.c
 * @authors Satish Nair
 * @version V1.0.0
 * @date    23-Dec-2014
 * @brief
 ******************************************************************************
  Copyright (c) 2013-2015 Particle Industries, Inc.  All rights reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation, either
  version 3 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************
 */

/* Includes ------------------------------------------------------------------*/
#include "pwm_hal.h"
#include "gpio_hal.h"
#include "pinmap_impl.h"

/* Private typedef -----------------------------------------------------------*/

/* Private define ------------------------------------------------------------*/

#define TIM_PWM_COUNTER_CLOCK_FREQ 30000000 //TIM Counter clock = 30MHz

/* Private macro -------------------------------------------------------------*/

/* Private variables ---------------------------------------------------------*/

/* Extern variables ----------------------------------------------------------*/

/* Private function prototypes -----------------------------------------------*/

uint32_t HAL_PWM_Enable_TIM_Clock(uint16_t pin);
uint32_t HAL_PWM_Calculate_Target_Clock(uint16_t pwm_frequency);
uint16_t HAL_PWM_Calculate_ARR(uint16_t pwm_frequency);
uint16_t HAL_PWM_Get_ARR(uint16_t pin);
uint16_t HAL_PWM_Calculate_CCR(uint32_t TIM_CLK, uint16_t period, uint8_t value);
uint32_t HAL_PWM_Base_Clock(uint16_t pin);
uint16_t HAL_PWM_Calculate_Prescaler(uint32_t TIM_CLK, uint32_t clock_freq);
void HAL_PWM_Update_Disable_Event(uint16_t pin, int enabled);
void HAL_PWM_Configure_TIM(uint32_t TIM_CLK, uint8_t value, uint16_t pin);
void HAL_PWM_Enable_TIM(uint16_t pin);
void HAL_PWM_Update_Duty_Cycle(uint16_t pin, uint16_t value);
void HAL_PWM_Update_DC_Frequency(uint16_t pin, uint16_t value, uint16_t pwm_frequency);

/*
 * @brief Should take an integer 0-255 and create a PWM signal with a duty cycle from 0-100%.
 * TIM_PWM_FREQ is set at 500 Hz
 */

void HAL_PWM_Write(uint16_t pin, uint8_t value)
{
	HAL_PWM_Write_With_Frequency(pin, value, TIM_PWM_FREQ);
}


/*
 * @brief Should take an integer 0-255 and create a PWM signal with a duty cycle from 0-100%
 * and a specified frequency.
 */

void HAL_PWM_Write_With_Frequency(uint16_t pin, uint8_t value, uint16_t pwm_frequency)
{
    STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

    // If PWM has not been initialized, or user has called pinMode(, OUTPUT)
    if (!(pin_info->user_property & PWM_INIT) || pin_info->pin_mode == OUTPUT)
    {
    	// Mark the initialization
    	pin_info->user_property |= PWM_INIT;

		// Save the frequency
		pin_info->pwm_frequency = pwm_frequency;

    	// Configure TIM GPIO pins
        HAL_Pin_Mode(pin, AF_OUTPUT_PUSHPULL);

        // Enable TIM clock
        uint32_t TIM_CLK = HAL_PWM_Enable_TIM_Clock(pin);

        // Configure Timer
    	HAL_PWM_Configure_TIM(TIM_CLK, value, pin);

    	// TIM enable counter
    	HAL_PWM_Enable_TIM(pin);
    }
    else
    {
    	HAL_PWM_Update_DC_Frequency(pin, value, pwm_frequency);
    }
}


uint16_t HAL_PWM_Get_Frequency(uint16_t pin)
{
    STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

    if(!pin_info->timer_peripheral)
    {
        return 0;
	}

	uint32_t clock_freq = HAL_PWM_Calculate_Target_Clock(pin_info->pwm_frequency);
	uint16_t TIM_ARR = HAL_PWM_Get_ARR(pin);
    uint16_t PWM_Frequency = (uint16_t)(clock_freq / (TIM_ARR + 1));

    return PWM_Frequency;
}


uint16_t HAL_PWM_Get_AnalogValue(uint16_t pin)
{
    uint16_t TIM_CCR = 0;
    uint16_t TIM_ARR = 0;
    uint16_t PWM_AnalogValue = 0;

    STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

    if(pin_info->timer_ch == TIM_Channel_1)
    {
        TIM_CCR = pin_info->timer_peripheral->CCR1;
    }
    else if(pin_info->timer_ch == TIM_Channel_2)
    {
        TIM_CCR = pin_info->timer_peripheral->CCR2;
    }
    else if(pin_info->timer_ch == TIM_Channel_3)
    {
        TIM_CCR = pin_info->timer_peripheral->CCR3;
    }
    else if(pin_info->timer_ch == TIM_Channel_4)
    {
        TIM_CCR = pin_info->timer_peripheral->CCR4;
    }
    else
    {
        return PWM_AnalogValue;
    }

    TIM_ARR = HAL_PWM_Get_ARR(pin);
    PWM_AnalogValue = (uint16_t)(((TIM_CCR + 1) * 255) / (TIM_ARR + 1));

    return PWM_AnalogValue;
}

uint32_t HAL_PWM_Base_Clock(uint16_t pin)
{
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

	if(pin_info->timer_peripheral == TIM3 ||
		pin_info->timer_peripheral == TIM4 ||
		pin_info->timer_peripheral == TIM5)
	{
		return SystemCoreClock / 2;
	}
	else
	{
		return SystemCoreClock;
	}
}

uint16_t HAL_PWM_Calculate_Prescaler(uint32_t TIM_CLK, uint32_t clock_freq) {
	return (uint16_t) (TIM_CLK / clock_freq) - 1;
}

void HAL_PWM_Update_DC_Frequency(uint16_t pin, uint16_t value, uint16_t pwm_frequency)
{
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

	// Calculate new prescaler, period and output compare register value
	uint32_t TIM_CLK = HAL_PWM_Base_Clock(pin);
	uint32_t clock_freq = HAL_PWM_Calculate_Target_Clock(pin_info->pwm_frequency);
	uint16_t TIM_Prescaler = HAL_PWM_Calculate_Prescaler(TIM_CLK, clock_freq);
	uint16_t TIM_ARR = HAL_PWM_Calculate_ARR(pin_info->pwm_frequency);
	uint16_t TIM_CCR = HAL_PWM_Calculate_CCR(TIM_CLK, TIM_ARR, value);

	// Disable update events while updating registers
	// In case a PWM period ends, it will keep the current values
	HAL_PWM_Update_Disable_Event(pin, DISABLE);

	// Update output compare register value
	if (pin_info->timer_ch == TIM_Channel_1) {
		TIM_SetCompare1(pin_info->timer_peripheral, TIM_CCR);
	} else if (pin_info->timer_ch == TIM_Channel_2) {
		TIM_SetCompare2(pin_info->timer_peripheral, TIM_CCR);
	} else if (pin_info->timer_ch == TIM_Channel_3) {
		TIM_SetCompare3(pin_info->timer_peripheral, TIM_CCR);
	} else if (pin_info->timer_ch == TIM_Channel_4) {
		TIM_SetCompare4(pin_info->timer_peripheral, TIM_CCR);
	}

	TIM_SetAutoreload(pin_info->timer_peripheral, TIM_ARR);
	TIM_PrescalerConfig(pin_info->timer_peripheral, TIM_Prescaler, TIM_PSCReloadMode_Update);

	// Re-enable update events
	// At the next update event (end of timer period) the preload
	// registers will be copied to the shadow registers
	HAL_PWM_Update_Disable_Event(pin, ENABLE);
}


void HAL_PWM_Update_Disable_Event(uint16_t pin, int enabled) {
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

	TIM_UpdateDisableConfig(pin_info->timer_peripheral, enabled);
}


uint32_t HAL_PWM_Enable_TIM_Clock(uint16_t pin)
{
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

	// TIM clock enable
	if (pin_info->timer_peripheral == TIM1)
	{
		GPIO_PinAFConfig(pin_info->gpio_peripheral, pin_info->gpio_pin_source, GPIO_AF_TIM1);
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
	}
	else if (pin_info->timer_peripheral == TIM3)
	{
		GPIO_PinAFConfig(pin_info->gpio_peripheral, pin_info->gpio_pin_source, GPIO_AF_TIM3);
		RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
	}
	else if (pin_info->timer_peripheral == TIM4)
	{
		GPIO_PinAFConfig(pin_info->gpio_peripheral, pin_info->gpio_pin_source, GPIO_AF_TIM4);
		RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
	}
	else if (pin_info->timer_peripheral == TIM5)
	{
		GPIO_PinAFConfig(pin_info->gpio_peripheral, pin_info->gpio_pin_source, GPIO_AF_TIM5);
		RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);
	}
#if PLATFORM_ID == 10 // Electron
	else if(pin_info->timer_peripheral == TIM8)
	{
		GPIO_PinAFConfig(pin_info->gpio_peripheral, pin_info->gpio_pin_source, GPIO_AF_TIM8);
		RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM8, ENABLE);
	}
#endif

	uint32_t clock_freq = HAL_PWM_Calculate_Target_Clock(pin_info->pwm_frequency);
	uint32_t TIM_CLK = HAL_PWM_Base_Clock(pin);
	uint16_t TIM_Prescaler = (uint16_t) (TIM_CLK / clock_freq) - 1;
	uint16_t TIM_ARR = HAL_PWM_Calculate_ARR(pin_info->pwm_frequency);

	// Time base configuration
	TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = { 0 };
	TIM_TimeBaseStructure.TIM_Period = TIM_ARR;
	TIM_TimeBaseStructure.TIM_Prescaler = TIM_Prescaler;
	TIM_TimeBaseStructure.TIM_ClockDivision = 0;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInit(pin_info->timer_peripheral, &TIM_TimeBaseStructure);

	return TIM_CLK;
}


uint16_t HAL_PWM_Calculate_CCR(uint32_t TIM_CLK, uint16_t period, uint8_t value)
{
	// TIM Channel Duty Cycle(%) = (TIM_CCR / TIM_ARR + 1) * 100
	uint16_t TIM_CCR = (uint16_t) (value * (period + 1) / 255);

	return TIM_CCR;
}

uint32_t HAL_PWM_Calculate_Target_Clock(uint16_t pwm_frequency)
{
	if(pwm_frequency == 0)
	{
		return 0;
	}

    //  __builtin_clz is a GCC built-in function that counts the number of leading zeros
	uint16_t leading_zeros = __builtin_clz(pwm_frequency);
	uint16_t leading_zeros_default = __builtin_clz(TIM_PWM_FREQ);

	if(leading_zeros > leading_zeros_default) {
		return TIM_PWM_COUNTER_CLOCK_FREQ >> (leading_zeros - leading_zeros_default);
	} else {
		return TIM_PWM_COUNTER_CLOCK_FREQ;
	}
}

uint16_t HAL_PWM_Calculate_ARR(uint16_t pwm_frequency)
{
	uint32_t clock_freq = HAL_PWM_Calculate_Target_Clock(pwm_frequency);
	return (uint16_t) (clock_freq / pwm_frequency) - 1;
}

uint16_t HAL_PWM_Get_ARR(uint16_t pin)
{
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;
	return pin_info->timer_peripheral->ARR;
}

void HAL_PWM_Configure_TIM(uint32_t TIM_CLK, uint8_t value, uint16_t pin)
{
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

	//PWM Duty Cycle
	uint16_t TIM_ARR = HAL_PWM_Get_ARR(pin);
	uint16_t TIM_CCR = HAL_PWM_Calculate_CCR(TIM_CLK, TIM_ARR, value);

	// PWM1 Mode configuration
	// Initialize all 8 struct params to 0, fixes randomly inverted RX, TX PWM
	TIM_OCInitTypeDef TIM_OCInitStructure = { 0 };
	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
	TIM_OCInitStructure.TIM_Pulse = TIM_CCR;

	// Enable output-compare preload function.  Duty cycle will be updated
	// at end of each counter cycle to prevent glitches.
	if (pin_info->timer_ch == TIM_Channel_1)
	{
		// PWM1 Mode configuration: Channel1
		TIM_OC1Init(pin_info->timer_peripheral, &TIM_OCInitStructure);
		TIM_OC1PreloadConfig(pin_info->timer_peripheral, TIM_OCPreload_Enable);
	}
	else if (pin_info->timer_ch == TIM_Channel_2)
	{
		// PWM1 Mode configuration: Channel2
		TIM_OC2Init(pin_info->timer_peripheral, &TIM_OCInitStructure);
		TIM_OC2PreloadConfig(pin_info->timer_peripheral,TIM_OCPreload_Enable);
	}
	else if (pin_info->timer_ch == TIM_Channel_3)
	{
		// PWM1 Mode configuration: Channel3
		TIM_OC3Init(pin_info->timer_peripheral, &TIM_OCInitStructure);
		TIM_OC3PreloadConfig(pin_info->timer_peripheral, TIM_OCPreload_Enable);
	}
	else if (pin_info->timer_ch == TIM_Channel_4)
	{
		// PWM1 Mode configuration: Channel4
		TIM_OC4Init(pin_info->timer_peripheral, &TIM_OCInitStructure);
		TIM_OC4PreloadConfig(pin_info->timer_peripheral, TIM_OCPreload_Enable);
	}

	// Enable Auto-load register preload function.  ARR register or PWM period
	// will be update at end of each counter cycle to prevent glitches.
	TIM_ARRPreloadConfig(pin_info->timer_peripheral, ENABLE);
}


void HAL_PWM_Enable_TIM(uint16_t pin)
{
	STM32_Pin_Info* pin_info = HAL_Pin_Map() + pin;

	// TIM enable counter
	TIM_Cmd(pin_info->timer_peripheral, ENABLE);

	if ((pin_info->timer_peripheral == TIM1) ||
		(pin_info->timer_peripheral == TIM8))
	{
		/* TIM Main Output Enable - required for TIM1/TIM8 PWM output */
		TIM_CtrlPWMOutputs(pin_info->timer_peripheral, ENABLE);
	}
}
