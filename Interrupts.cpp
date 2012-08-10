#include <cstdint>

#include "ch.h"
#include "hal.h"

#include "EncoderTimers.h"
#include "Interrupts.h"
#include "SampleAndControl.h"

// There are 4 ways this interrupt gets called.
// 1) Timer overflow
// 2) Rising edge on IC1  ( Rear wheel encoder A)
// 3) Rising edge on IC2  (Front wheel encoder A)
// 4) Risign edge on IC3  (      Steer encoder A)
CH_IRQ_HANDLER(VectorB8)
{
  CH_IRQ_PROLOGUE();

  static uint32_t overflows[3] = { 0, 0, 0 }; // to store # of overflows per
                                              // per input channel
  static uint32_t CCR_prev[3] = { 0, 0, 0 };  // to store the value of CNT at
                                              // the time of the input capture
                                              // events
  static uint32_t pre_overflow_cnt[3] = {0, 0, 0};

  // uint32_t cnt = STM32_TIM4->CNT; // Save TIM4 count
  uint16_t sr = STM32_TIM4->SR;   // Save TIM4 status register
  uint32_t dir = GPIOA->IDR;      // Save GPIOA; encoders B channel state
  STM32_TIM4->SR = ~sr;           // Write zero to bits high at time of SR read
                                  // note that writing '1' has no effect on SR
                                  // because all bits are 'rc_w0': Software can
                                  // read as well as clear this bit by writing
                                  // 0.  Writing '1' has no effect on the bit
                                  // value.

  if (sr & 1) {                   // UE (overflow, since TIM4 is upcounting)
    // When the timer has overflowed, there are 2 situations we need to address
    // for each of the three channels being measured:
    // 
    // 1) This is the first overflow (overflows[i] == 0) to occur since the
    // last IC interrupt.  In this case, we need store 2^16 minus the value of
    // CNT when the last IC interrupt occurred.  We then increment overflows.
    //
    // 2) This is not the first overflow (overflows[i] > 0) to occur since the
    // last IC interrupt.  In this case, we simply increment overflows[i] and
    // leave pre_overflow_cnt[i] untouched.
    //
    // In both cases, we increment overflows[i], so this can be all wrapped
    // into a single for loop:
    
    for (int i = 0; i < 3; ++i) {
      if (overflows[i]++ == 0)  // check overflows before incrementing
        pre_overflow_cnt[i] = (1 << 16) - CCR_prev[i];
    }
  }

  // For IC1, IC2, IC3 (i = 0, 1, 2) events we need to do the following:
  // 1) If we haven't overflowed the counter, the Clockticks is simply the
  // clock value at the time of the IC event, CCR[i], minus the clock value at
  // the time of the previous event, CCR_prev[i].  If we have overflowed the
  // counter, the Clockticks is: 
  // pre_overflow_cnt[i] + (overflows[i] - 1)*2^16 + CCR[i],
  // 2) overflows[i] = 0;
  // 4) Save state of Encoder B; this indicates the direction:
  //    if (dir & (1 << (i + 12)))
  //      SampleAndControl::timers.Direction[i] = EncoderTimers::cw;
  //    else
  //      SampleAndControl::timers.Direction[i] = EncoderTimers::ccw;
  //    
  // All of this can be wrapped into a nice for loop:
  
  for (int i = 0; i < 3; ++i) {
    if (sr & (1 << (i + 1))) { // IC1, IC2, IC3 are on bits 1, 2, 3 of SR

      // Clock ticks since last rising edge
      uint32_t tmp = STM32_TIM4->CCR[i]; // obtain count since last overflow

      if (overflows[i] == 0) {
        SampleAndControl::timers.Clockticks[i] = tmp - CCR_prev[i];
      } else {
        SampleAndControl::timers.Clockticks[i] = pre_overflow_cnt[i]
                                               + (overflows[i] - 1) * (1 << 16)
                                               + tmp;
        overflows[i] = 0;     // clear the overflow count
      }
      CCR_prev[i] = tmp;      // save the capture compare register

      // Direction
      if (dir & (1 << i))     // Encoder B lines are on PA0, PA1, PA2
        SampleAndControl::timers.Direction[i] = EncoderTimers::cw;
      else                    // Enocder B line is low
        SampleAndControl::timers.Direction[i] = EncoderTimers::ccw;
    }
  }

  CH_IRQ_EPILOGUE();
}
