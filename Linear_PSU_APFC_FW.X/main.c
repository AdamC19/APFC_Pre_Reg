/**
  Generated main.c file from MPLAB Code Configurator

  @Company
    Microchip Technology Inc.

  @File Name
    main.c

  @Summary
    This is the generated main.c using PIC24 / dsPIC33 / PIC32MM MCUs.

  @Description
    This source file provides main entry point for system initialization and application code development.
    Generation Information :
        Product Revision  :  PIC24 / dsPIC33 / PIC32MM MCUs - 1.171.1
        Device            :  dsPIC33CK64MP502
    The generated drivers are tested against the following:
        Compiler          :  XC16 v1.70
        MPLAB 	          :  MPLAB X v5.50
*/

/*
    (c) 2020 Microchip Technology Inc. and its subsidiaries. You may use this
    software and any derivatives exclusively with Microchip products.

    THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
    EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY IMPLIED
    WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS FOR A
    PARTICULAR PURPOSE, OR ITS INTERACTION WITH MICROCHIP PRODUCTS, COMBINATION
    WITH ANY OTHER PRODUCTS, OR USE IN ANY APPLICATION.

    IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
    INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
    WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP HAS
    BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO THE
    FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL CLAIMS IN
    ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT OF FEES, IF ANY,
    THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS SOFTWARE.

    MICROCHIP PROVIDES THIS SOFTWARE CONDITIONALLY UPON YOUR ACCEPTANCE OF THESE
    TERMS.
*/

/**
  Section: Included Files
*/

#include <xc.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "mcc_generated_files/system.h"
#include "mcc_generated_files/adc1.h"
#include "mcc_generated_files/pwm.h"

typedef enum {
  STATE_IDLE,
  STATE_SOFT_START_INIT,
  STATE_SOFT_START,
  STATE_RUN_INIT,
  STATE_RUN
}state_t;

#define MIN_PWM_COUNTS      100
#define PWM_PERIOD_COUNTS   2000
#define MAX_PWM_COUNTS      (PWM_PERIOD_COUNTS - MIN_PWM_COUNTS)
#define VREF_MV             3300
#define VOUT_DIV            6
#define I_SHAPE_DIV         6
#define I_AMP_GAIN          50
#define INV_SHUNT_R         100
#define ADC_SAMPLE_SETS     4
#define VSET_STEP           100
#define KP                  100
#define KI                  5
#define MAX_ALLOWED_ERR     1500
#define VOUT_ERR_MAX_MV     19800
#define VOUT_ERR_INTEG_MAX  (VOUT_ERR_MAX_MV << KI)
#define I_SNS_MV_TO_MA(value)   ((value/I_AMP_GAIN)*INV_SHUNT_R)
#define CLAMP(value, min, max)  (value < min ? min : (value > max ? max : value))
#define WITHIN_RANGE(value, target, range)  (value > target - range ? (value < target + range) : false)

state_t state = STATE_RUN_INIT;
volatile unsigned int vout_setpt_mv  = 0;
volatile unsigned int vout_target_mv = 0;
volatile int vout_err_mv             = 0;
volatile unsigned int i_shape_counts = 0;
volatile unsigned int vin_mv         = 0;
volatile unsigned int vout_mv        = 0;
volatile unsigned int i_sns_mv       = 0;
volatile unsigned int i_sns_ma       = 0;
volatile uint32_t adc_conv           = 0;
volatile int adc_sample_counter      = 0;
volatile bool adc_samples_rdy        = false;
volatile unsigned int duty_cycle     = MIN_PWM_COUNTS;
volatile uint32_t vout_err_integ     = 0;
volatile unsigned int vout_duty_ctrl = 0;

void vout_setpt_callback();
void i_shape_callback();
void vout_callback();
void i_sns_callback();
long map(long x, long in_min, long in_max, long out_min, long out_max);

/*
                         Main application
 */
int main(void)
{
    // initialize the device
    SYSTEM_Initialize();

    ADC1_SetVOUT_SETPTInterruptHandler(vout_setpt_callback);
    ADC1_SetI_SHAPEInterruptHandler(i_shape_callback);
    ADC1_SetVOUT_FBInterruptHandler(vout_callback);
    ADC1_SetI_SNSInterruptHandler(i_sns_callback);

    ADC1_Enable();
      
    ADC1_ChannelSelect(VOUT_SETPT);
    ADC1_SoftwareTriggerEnable();

    while (1)
    {
      // Add your application code
      
      // do all the other things
      if(adc_sample_counter >= ADC_SAMPLE_SETS){
        adc_sample_counter = 0;

        // Adjust Vout Setpoint
        int v_adj = vout_target_mv - vout_setpt_mv;
        v_adj = CLAMP(v_adj, -VSET_STEP, VSET_STEP);
        vout_setpt_mv += v_adj;
        
        switch(state){
          case STATE_IDLE:{
            PWM_GeneratorDisable(PWM_GENERATOR_1);
            vout_setpt_mv = 0;
          }
          case STATE_SOFT_START_INIT:{
            PWM_DutyCycleSet(PWM_GENERATOR_1, MIN_PWM_COUNTS);
            PWM_GeneratorEnable(PWM_GENERATOR_1);
            state = STATE_SOFT_START;
            break;
          }case STATE_SOFT_START:{
            if(WITHIN_RANGE(vout_setpt_mv, vout_target_mv, 50)){
              // wait for the Vout setpt input to read something reasonable
              // technically the miniumum we should ever be able to see is ~71mV (426mV at output)
              state = STATE_RUN_INIT;
            }
            break;
          }case STATE_RUN_INIT:{
            duty_cycle = MIN_PWM_COUNTS;
            PWM_DutyCycleSet(PWM_GENERATOR_1, duty_cycle);
            PWM_GeneratorEnable(PWM_GENERATOR_1);
            state = STATE_RUN;
            break;
          }case STATE_RUN:{
            
            vout_err_mv = CLAMP(vout_setpt_mv - vout_mv, -VOUT_ERR_MAX_MV, VOUT_ERR_MAX_MV); 
            vout_err_integ += vout_err_mv; // pretty slow-changing 
            vout_err_integ = CLAMP(vout_err_integ, 0, VOUT_ERR_INTEG_MAX);
            int err_total = CLAMP(vout_err_mv + (vout_err_integ >> KI), 0, VOUT_ERR_MAX_MV);

            // it's safe to say that error is going to change little over a half cycle
            // scale shape signal by err_total 
            uint32_t i_setpt = (err_total*i_shape_counts)/MAX_ALLOWED_ERR;
            long i_setpt = map(vout_err_mv, 0, MAX_ALLOWED_ERR, 0, i_shape_counts);
            uint32_t vout_err_p = vout_err_mv * i_shape_counts;
            break;
          }
          default:{
            state = STATE_SOFT_START_INIT;
            break;
          }
        }
        // debug here
      }

    }
    return 1; 
}


void vout_setpt_callback(){
  adc_conv = ADC1_ConversionResultGet(VOUT_SETPT);
  vout_target_mv = VOUT_DIV*((adc_conv*VREF_MV) >> 12);
  ADC1_ChannelSelect(I_SHAPE);
  ADC1_SoftwareTriggerEnable();
}

void i_shape_callback(){
  adc_conv = ADC1_ConversionResultGet(I_SHAPE);
  i_shape_counts = (unsigned int) adc_conv;
  vin_mv = VOUT_DIV*((i_shape_counts * VREF_MV) >> 12);
  ADC1_ChannelSelect(VOUT_FB);
  ADC1_SoftwareTriggerEnable();
}

void vout_callback(){
  adc_conv = ADC1_ConversionResultGet(VOUT_FB);
  vout_mv = VOUT_DIV*((adc_conv * VREF_MV) >> 12);
  ADC1_ChannelSelect(I_SNS);
  ADC1_SoftwareTriggerEnable();
}

void i_sns_callback(){
  adc_conv = ADC1_ConversionResultGet(I_SNS);
  i_sns_mv = (adc_conv*VREF_MV) >> 12;
  i_sns_ma = I_SNS_MV_TO_MA(i_sns_mv);
  ADC1_ChannelSelect(VOUT_SETPT);
  ADC1_SoftwareTriggerEnable();
  adc_samples_rdy = true; // all 4 relevant samples have been taken
  adc_sample_counter++;
}

long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/**
 End of File
*/

