//----------------------------------------------------------------------//
// Description: stepgen.c                                               //
// Code to interface to a PRU driven step generator                     //
//                                                                      //
// Author(s): Charles Steinkuehler                                      //
// License: GNU GPL Version 2.0 or (at your option) any later version.  //
//                                                                      //
// Last change:                                                         //
// 2012-Dec-30 Charles Steinkuehler                                     //
//             Initial version, based in part on:                       //
//               hal_pru.c      Micheal Halberler                       //
//               supply.c       Matt Shaver                             //
//               stepgen.c      John Kasunich                           //
//               hostmot2 code  Sebastian Kuzminsky                     //
//----------------------------------------------------------------------//
// This file is part of LinuxCNC HAL                                    //
//                                                                      //
// Copyright (C) 2012  Charles Steinkuehler                             //
//                     <charles AT steinkuehler DOT net>                //
//                                                                      //
// This program is free software; you can redistribute it and/or        //
// modify it under the terms of the GNU General Public License          //
// as published by the Free Software Foundation; either version 2       //
// of the License, or (at your option) any later version.               //
//                                                                      //
// This program is distributed in the hope that it will be useful,      //
// but WITHOUT ANY WARRANTY; without even the implied warranty of       //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the        //
// GNU General Public License for more details.                         //
//                                                                      //
// You should have received a copy of the GNU General Public License    //
// along with this program; if not, write to the Free Software          //
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA        //
// 02110-1301, USA.                                                     //
//                                                                      //
// THE AUTHORS OF THIS PROGRAM ACCEPT ABSOLUTELY NO LIABILITY FOR       //
// ANY HARM OR LOSS RESULTING FROM ITS USE.  IT IS _EXTREMELY_ UNWISE   //
// TO RELY ON SOFTWARE ALONE FOR SAFETY.  Any machinery capable of      //
// harming persons must have provisions for completely removing power   //
// from all motors, etc, before persons enter any danger area.  All     //
// machinery must be designed to comply with local and national safety  //
// codes, and the authors of this software can not, and do not, take    //
// any responsibility for such compliance.                              //
//                                                                      //
// This code was written as part of the LinuxCNC project.  For more     //
// information, go to www.linuxcnc.org.                                 //
//----------------------------------------------------------------------//

// Use config_module.h instead of config.h so we can use RTAPI_INC_LIST_H
#include "config_module.h"

// this probably should be an ARM335x #define
#if !defined(TARGET_PLATFORM_BEAGLEBONE)
#error "This driver is for the beaglebone platform only"
#endif

#if !defined(BUILD_SYS_USER_DSO)
#error "This driver is for usermode threads only"
#endif

// #include RTAPI_INC_LIST_H
// #include "rtapi.h"          /* RTAPI realtime OS API */
// #include "rtapi_app.h"      /* RTAPI realtime module decls */
// #include "rtapi_math.h"
// #include "hal.h"            /* HAL public API decls */
// #include <pthread.h>
// 
// #include <stdio.h>
// #include <stdlib.h>
// #include <unistd.h>
// #include <sys/types.h>

#include "rtapi.h"
#include "rtapi_app.h"
#include "rtapi_string.h"
#include "rtapi_math.h"

#include "hal.h"

#include "hal/drivers/hal_pru_generic/hal_pru_generic.h"


#define f_period_s ((double)(l_period_ns * 1e-9))


// Start out with default pulse length/width and setup/hold delays of 1 mS (1000000 nS) 
#define DEFAULT_DELAY 1000000


/***********************************************************************
*                       REALTIME FUNCTIONS                             *
************************************************************************/
// 
// read accumulator to figure out where the stepper has gotten to
// 

static void hpg_read(void *void_hpg, long l_period_ns) {
    // Read data from the PRU here...
    hal_pru_generic_t *hpg = void_hpg;
    int i;

    for (i = 0; i < hpg->stepgen.num_instances; i ++) {
        s64 x, y;
        u32 acc;
        s64 acc_delta;

        // "atomic" read of accumulator and position register from PRU
	    do {
            x = * (s64 *) hpg->pru_data + hpg->stepgen.instance[i].task.addr + offsetof(PRU_task_stepdir_t, accum);
            y = * (s64 *) hpg->pru_data + hpg->stepgen.instance[i].task.addr + offsetof(PRU_task_stepdir_t, accum);
	    } while ( x != y );

        // Update internal state
        * (s64 *) &hpg->stepgen.instance[i].PRU.accum = x;

*(hpg->stepgen.instance[i].hal.pin.test1) = hpg->stepgen.instance[i].PRU.accum;
*(hpg->stepgen.instance[i].hal.pin.test2) = hpg->stepgen.instance[i].PRU.pos;

        // Mangle 32-bit step count and 27 bit accumulator (with 5 bits of status)
        // into a 16.16 value to match the hostmot2 stepgen logic and generally make
        // things less confusing
        acc  = (hpg->stepgen.instance[i].PRU.accum >> 11) & 0x0000FFFF;
        acc |= (hpg->stepgen.instance[i].PRU.pos << 16);

*(hpg->stepgen.instance[i].hal.pin.test3) = acc;

        // those tricky users are always trying to get us to divide by zero
        if (fabs(hpg->stepgen.instance[i].hal.param.position_scale) < 1e-6) {
            if (hpg->stepgen.instance[i].hal.param.position_scale >= 0.0) {
                hpg->stepgen.instance[i].hal.param.position_scale = 1.0;
                HPG_ERR("stepgen %d position_scale is too close to 0, resetting to 1.0\n", i);
            } else {
                hpg->stepgen.instance[i].hal.param.position_scale = -1.0;
                HPG_ERR("stepgen %d position_scale is too close to 0, resetting to -1.0\n", i);
            }
        }

        // The HM2 Accumulator Register is a 16.16 bit fixed-point
        // representation of the current stepper position.
        // The fractional part gives accurate velocity at low speeds, and
        // sub-step position feedback (like sw stepgen).
        acc_delta = (s64)acc - (s64)hpg->stepgen.instance[i].prev_accumulator;
        if (acc_delta > INT32_MAX) {
            acc_delta -= UINT32_MAX;
        } else if (acc_delta < INT32_MIN) {
            acc_delta += UINT32_MAX;
        }

        hpg->stepgen.instance[i].subcounts += acc_delta;

        *(hpg->stepgen.instance[i].hal.pin.counts) = hpg->stepgen.instance[i].subcounts >> 16;

        // note that it's important to use "subcounts/65536.0" instead of just
        // "counts" when computing position_fb, because position_fb needs sub-count
        // precision
        *(hpg->stepgen.instance[i].hal.pin.position_fb) = ((double)hpg->stepgen.instance[i].subcounts / 65536.0) / hpg->stepgen.instance[i].hal.param.position_scale;

        hpg->stepgen.instance[i].prev_accumulator = acc;

    }
}

static void hm2_stepgen_instance_position_control(hal_pru_generic_t *hpg, long l_period_ns, int i, double *new_vel) {
    double ff_vel;
    double velocity_error;
    double match_accel;
    double seconds_to_vel_match;
    double position_at_match;
    double position_cmd_at_match;
    double error_at_match;
    double velocity_cmd;

    (*hpg->stepgen.instance[i].hal.pin.dbg_pos_minus_prev_cmd) = (*hpg->stepgen.instance[i].hal.pin.position_fb) - hpg->stepgen.instance[i].old_position_cmd;

    // calculate feed-forward velocity in machine units per second
    ff_vel = ((*hpg->stepgen.instance[i].hal.pin.position_cmd) - hpg->stepgen.instance[i].old_position_cmd) / f_period_s;
    (*hpg->stepgen.instance[i].hal.pin.dbg_ff_vel) = ff_vel;

    hpg->stepgen.instance[i].old_position_cmd = (*hpg->stepgen.instance[i].hal.pin.position_cmd);

    velocity_error = (*hpg->stepgen.instance[i].hal.pin.velocity_fb) - ff_vel;
    (*hpg->stepgen.instance[i].hal.pin.dbg_vel_error) = velocity_error;

    // Do we need to change speed to match the speed of position-cmd?
    // If maxaccel is 0, there's no accel limit: fix this velocity error
    // by the next servo period!  This leaves acceleration control up to
    // the trajectory planner.
    // If maxaccel is not zero, the user has specified a maxaccel and we
    // adhere to that.
    if (velocity_error > 0.0) {
        if (hpg->stepgen.instance[i].hal.param.maxaccel == 0) {
            match_accel = -velocity_error / f_period_s;
        } else {
            match_accel = -hpg->stepgen.instance[i].hal.param.maxaccel;
        }
    } else if (velocity_error < 0.0) {
        if (hpg->stepgen.instance[i].hal.param.maxaccel == 0) {
            match_accel = velocity_error / f_period_s;
        } else {
            match_accel = hpg->stepgen.instance[i].hal.param.maxaccel;
        }
    } else {
        match_accel = 0;
    }

    if (match_accel == 0) {
        // vel is just right, dont need to accelerate
        seconds_to_vel_match = 0.0;
    } else {
        seconds_to_vel_match = -velocity_error / match_accel;
    }
    *hpg->stepgen.instance[i].hal.pin.dbg_s_to_match = seconds_to_vel_match;

    // compute expected position at the time of velocity match
    // Note: this is "feedback position at the beginning of the servo period after we attain velocity match"
    {
        double avg_v;
        avg_v = (ff_vel + *hpg->stepgen.instance[i].hal.pin.velocity_fb) * 0.5;
        position_at_match = *hpg->stepgen.instance[i].hal.pin.position_fb + (avg_v * (seconds_to_vel_match + f_period_s));
    }

    // Note: this assumes that position-cmd keeps the current velocity
    position_cmd_at_match = *hpg->stepgen.instance[i].hal.pin.position_cmd + (ff_vel * seconds_to_vel_match);
    error_at_match = position_at_match - position_cmd_at_match;

    *hpg->stepgen.instance[i].hal.pin.dbg_err_at_match = error_at_match;

    if (seconds_to_vel_match < f_period_s) {
        // we can match velocity in one period
        // try to correct whatever position error we have
        velocity_cmd = ff_vel - (0.5 * error_at_match / f_period_s);

        // apply accel limits?
        if (hpg->stepgen.instance[i].hal.param.maxaccel > 0) {
            if (velocity_cmd > (*hpg->stepgen.instance[i].hal.pin.velocity_fb + (hpg->stepgen.instance[i].hal.param.maxaccel * f_period_s))) {
                velocity_cmd = *hpg->stepgen.instance[i].hal.pin.velocity_fb + (hpg->stepgen.instance[i].hal.param.maxaccel * f_period_s);
            } else if (velocity_cmd < (*hpg->stepgen.instance[i].hal.pin.velocity_fb - (hpg->stepgen.instance[i].hal.param.maxaccel * f_period_s))) {
                velocity_cmd = *hpg->stepgen.instance[i].hal.pin.velocity_fb - (hpg->stepgen.instance[i].hal.param.maxaccel * f_period_s);
            }
        }

    } else {
        // we're going to have to work for more than one period to match velocity
        // FIXME: I dont really get this part yet

        double dv;
        double dp;

        /* calculate change in final position if we ramp in the opposite direction for one period */
        dv = -2.0 * match_accel * f_period_s;
        dp = dv * seconds_to_vel_match;

        /* decide which way to ramp */
        if (fabs(error_at_match + (dp * 2.0)) < fabs(error_at_match)) {
            match_accel = -match_accel;
        }

        /* and do it */
        velocity_cmd = *hpg->stepgen.instance[i].hal.pin.velocity_fb + (match_accel * f_period_s);
    }

    *new_vel = velocity_cmd;
}

// This function was invented by Jeff Epler.
// It forces a floating-point variable to be degraded from native register
// size (80 bits on x86) to C double size (64 bits).
static double force_precision(double d) __attribute__((__noinline__));
static double force_precision(double d) {
    return d;
}

static void update_stepgen(hal_pru_generic_t *hpg, long l_period_ns, int i) {
    double new_vel;

    double physical_maxvel;  // max vel supported by current step timings & position-scale
    double maxvel;           // actual max vel to use this time

    double steps_per_sec_cmd;

    hpg_stepgen_instance_t *s = &hpg->stepgen.instance[i];


    //
    // first sanity-check our maxaccel and maxvel params
    //

    // maxvel must be >= 0.0, and may not be faster than 1 step per (steplen+stepspace) seconds
    {
        double min_ns_per_step = s->hal.param.steplen + s->hal.param.stepspace;
        double max_steps_per_s = 1.0e9 / min_ns_per_step;

        physical_maxvel = max_steps_per_s / fabs(s->hal.param.position_scale);
        physical_maxvel = force_precision(physical_maxvel);

        if (s->hal.param.maxvel < 0.0) {
            HPG_ERR("stepgen.%02d.maxvel < 0, setting to its absolute value\n", i);
            s->hal.param.maxvel = fabs(s->hal.param.maxvel);
        }

        if (s->hal.param.maxvel > physical_maxvel) {
            HPG_ERR("stepgen.%02d.maxvel is too big for current step timings & position-scale, clipping to max possible\n", i);
            s->hal.param.maxvel = physical_maxvel;
        }

        if (s->hal.param.maxvel == 0.0) {
            maxvel = physical_maxvel;
        } else {
            maxvel = s->hal.param.maxvel;
        }
    }

    // maxaccel may not be negative
    if (s->hal.param.maxaccel < 0.0) {
        HPG_ERR("stepgen.%02d.maxaccel < 0, setting to its absolute value\n", i);
        s->hal.param.maxaccel = fabs(s->hal.param.maxaccel);
    }


    // select the new velocity we want
    if (*s->hal.pin.control_type == 0) {
        hm2_stepgen_instance_position_control(hpg, l_period_ns, i, &new_vel);
    } else {
        // velocity-mode control is easy
        new_vel = *s->hal.pin.velocity_cmd;
        if (s->hal.param.maxaccel > 0.0) {
            if (((new_vel - *s->hal.pin.velocity_fb) / f_period_s) > s->hal.param.maxaccel) {
                new_vel = (*s->hal.pin.velocity_fb) + (s->hal.param.maxaccel * f_period_s);
            } else if (((new_vel - *s->hal.pin.velocity_fb) / f_period_s) < -s->hal.param.maxaccel) {
                new_vel = (*s->hal.pin.velocity_fb) - (s->hal.param.maxaccel * f_period_s);
            }
        }
    }

    // clip velocity to maxvel
    if (new_vel > maxvel) {
        new_vel = maxvel;
    } else if (new_vel < -maxvel) {
        new_vel = -maxvel;
    }


    *s->hal.pin.velocity_fb = (hal_float_t)new_vel;

    steps_per_sec_cmd = new_vel * s->hal.param.position_scale;
    s->PRU.rate = steps_per_sec_cmd * (double)0x8000000 * (double) hpg->config.pru_period * 1e-9;
    
    // clip rate just to be safe...should be limited by code above
/*    if (s->PRU.rate > 0x03FFFFFF) {
        s->PRU.rate = 0x03FFFFFF;
    } else if (s->PRU.rate < 0xFC000001) {
        s->PRU.rate = 0xFC000001;
    }
*/
    *s->hal.pin.dbg_step_rate = s->PRU.rate;
}

static void hpg_write(void *void_hpg, long period) {
    hal_pru_generic_t *hpg      = void_hpg;
//     PRU_chan_state_ptr pru      = (PRU_chan_state_ptr) pru_data_ram;
//     int i, j;
// 
//     for (i = 0; i < MAX_CHAN; i++) {
//         switch (chan_state[i].gen.PRU.ctrl.mode) {
// 
//         case eMODE_STEP_DIR :
// 
//             // Update shadow of PRU control registers
//             chan_state[i].step.PRU.ctrl.enable  = *(chan_state[i].step.hal.pin.enable);
//             chan_state[i].step.PRU.ctrl.pin1    = chan_state[i].step.hal.param.steppin;
//             chan_state[i].step.PRU.ctrl.pin2    = chan_state[i].step.hal.param.dirpin;
// 
//             if (*(chan_state[i].step.hal.pin.enable) == 0) {
//                 chan_state[i].step.PRU.rate = 0;
//                 chan_state[i].step.old_position_cmd = *(chan_state[i].step.hal.pin.position_cmd);
//                 *(chan_state[i].step.hal.pin.velocity_fb) = 0;
//             } else {
//                 // call update function
//                 update_stepgen(hpg, period, i);
//             }
// 
//             // Update timing parameters if changed
//             if ((chan_state[i].step.hal.param.dirsetup  != chan_state[i].step.written_dirsetup ) ||
//                 (chan_state[i].step.hal.param.dirhold   != chan_state[i].step.written_dirhold  ) ||
//                 (chan_state[i].step.hal.param.steplen   != chan_state[i].step.written_steplen  ) ||
//                 (chan_state[i].step.hal.param.stepspace != chan_state[i].step.written_stepspace))
//             {
//                 chan_state[i].step.PRU.dirsetup     = ns2periods(chan_state[i].step.hal.param.dirsetup);
//                 chan_state[i].step.PRU.dirhold      = ns2periods(chan_state[i].step.hal.param.dirhold);
//                 chan_state[i].step.PRU.steplen      = ns2periods(chan_state[i].step.hal.param.steplen);
//                 chan_state[i].step.PRU.stepspace    = ns2periods(chan_state[i].step.hal.param.stepspace);
// 
//                 // Send new value(s) to the PRU
//                 pru[i].raw.dword[2] = chan_state[i].raw.PRU.dword[2];
//                 pru[i].raw.dword[3] = chan_state[i].raw.PRU.dword[3];
// 
//                 // Stash values written
//                 chan_state[i].step.written_dirsetup  = chan_state[i].step.hal.param.dirsetup;
//                 chan_state[i].step.written_dirhold   = chan_state[i].step.hal.param.dirhold;
//                 chan_state[i].step.written_steplen   = chan_state[i].step.hal.param.steplen;
//                 chan_state[i].step.written_stepspace = chan_state[i].step.hal.param.stepspace;
//             }
// 
//             // Update control word if changed
//             if (chan_state[i].raw.PRU.dword[0] != chan_state[i].step.written_ctrl) {
//                 pru[i].raw.dword[0] = chan_state[i].raw.PRU.dword[0];
//                 chan_state[i].step.written_ctrl = chan_state[i].raw.PRU.dword[0];
//             }
// 
//             // Send rate update to the PRU
//             pru[i].step.rate = chan_state[i].step.PRU.rate;
// 
//             break;
// 
//         case eMODE_DELTA_SIG :
// 
//             // Update shadow of PRU control registers
//             chan_state[i].delta.PRU.ctrl.enable  = *(chan_state[i].delta.hal_enable);
// 
//             if (*(chan_state[i].delta.hal_out1) >= 1.0) {
//                 chan_state[i].delta.PRU.value1 = 0x4000;
//             } else if (*(chan_state[i].delta.hal_out1) <= 0.0) {
//                 chan_state[i].delta.PRU.value1 = 0x0000;
//             } else {
//                 chan_state[i].delta.PRU.value1 = 
//                     (u32) (*(chan_state[i].delta.hal_out1) * (1 << 14)) & 0x3FFF;
//             }
// 
//             if (*(chan_state[i].delta.hal_out2) == 1.0) {
//                 chan_state[i].delta.PRU.value2 = 0x4000;
//             } else if (*(chan_state[i].delta.hal_out2) <= 0.0) {
//                 chan_state[i].delta.PRU.value2 = 0x0000;
//             } else {
//                 chan_state[i].delta.PRU.value2 =
//                     (u32) (*(chan_state[i].delta.hal_out2) * (1 << 14)) & 0x3FFF;
//             }
// 
//             chan_state[i].delta.PRU.ctrl.pin1   = chan_state[i].delta.hal_pin1;
//             chan_state[i].delta.PRU.ctrl.pin2   = chan_state[i].delta.hal_pin2;
// 
//             // Send updates to PRU
//             for (j = 0; j < 2; j++) {
//                 pru[i].raw.dword[j] = chan_state[i].raw.PRU.dword[j];
//             }
//             break;
// 
//         case eMODE_PWM :
// 
//             // Update shadow of PRU control registers
//             chan_state[i].pwm.PRU.ctrl.enable   = *(chan_state[i].pwm.hal_enable);
//             chan_state[i].pwm.PRU.period        = *(chan_state[i].pwm.hal_period);
//             chan_state[i].pwm.PRU.high1         = *(chan_state[i].pwm.hal_out1);
//             chan_state[i].pwm.PRU.high2         = *(chan_state[i].pwm.hal_out2);
// 
//             chan_state[i].pwm.PRU.ctrl.pin1     = chan_state[i].pwm.hal_pin1;
//             chan_state[i].pwm.PRU.ctrl.pin2     = chan_state[i].pwm.hal_pin2;
// 
//             // Send updates to PRU
//             for (j = 0; j < 4; j++) {
//                 pru[i].raw.dword[j] = chan_state[i].raw.PRU.dword[j];
//             }
//             break;
// 
//         default :
//             // Nothing to export for other types
//             break;
//         }
//     }
}


//
// Here's the stepgen position controller.  It uses first-order
// feedforward and proportional error feedback.  This code is based
// on John Kasunich's software stepgen code.
//

int export_stepgen(hal_pru_generic_t *hpg, int i)
{
    char name[HAL_NAME_LEN + 1];
    int r;

    // Pins
    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.position-cmd", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_IN, &(hpg->stepgen.instance[i].hal.pin.position_cmd), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.velocity-cmd", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_IN, &(hpg->stepgen.instance[i].hal.pin.velocity_cmd), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.velocity-fb", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.velocity_fb), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.position-fb", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.position_fb), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.counts", hpg->config.name, i);
    r = hal_pin_s32_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.counts), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.enable", hpg->config.name, i);
    r = hal_pin_bit_new(name, HAL_IN, &(hpg->stepgen.instance[i].hal.pin.enable), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.control-type", hpg->config.name, i);
    r = hal_pin_bit_new(name, HAL_IN, &(hpg->stepgen.instance[i].hal.pin.control_type), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    // debug pins

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dbg_pos_minus_prev_cmd", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.dbg_pos_minus_prev_cmd), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dbg_ff_vel", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.dbg_ff_vel), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dbg_s_to_match", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.dbg_s_to_match), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dbg_vel_error", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.dbg_vel_error), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dbg_err_at_match", hpg->config.name, i);
    r = hal_pin_float_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.dbg_err_at_match), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dbg_step_rate", hpg->config.name, i);
    r = hal_pin_s32_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.dbg_step_rate), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.test1", hpg->config.name, i);
    r = hal_pin_s32_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.test1), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.test2", hpg->config.name, i);
    r = hal_pin_s32_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.test2), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.test3", hpg->config.name, i);
    r = hal_pin_s32_new(name, HAL_OUT, &(hpg->stepgen.instance[i].hal.pin.test3), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding pin '%s', aborting\n", name);
        return r;
    }

    // Parameters
    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.position-scale", hpg->config.name, i);
    r = hal_param_float_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.position_scale), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.maxvel", hpg->config.name, i);
    r = hal_param_float_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.maxvel), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.maxaccel", hpg->config.name, i);
    r = hal_param_float_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.maxaccel), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.steplen", hpg->config.name, i);
    r = hal_param_u32_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.steplen), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.stepspace", hpg->config.name, i);
    r = hal_param_u32_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.stepspace), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dirsetup", hpg->config.name, i);
    r = hal_param_u32_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.dirsetup), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dirhold", hpg->config.name, i);
    r = hal_param_u32_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.dirhold), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.steppin", hpg->config.name, i);
    r = hal_param_u32_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.steppin), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    rtapi_snprintf(name, sizeof(name), "%s.stepgen.%02d.dirpin", hpg->config.name, i);
    r = hal_param_u32_new(name, HAL_RW, &(hpg->stepgen.instance[i].hal.param.dirpin), hpg->config.comp_id);
    if (r < 0) {
        HPG_ERR("Error adding param '%s', aborting\n", name);
        return r;
    }

    // init
    *(hpg->stepgen.instance[i].hal.pin.position_cmd) = 0.0;
    *(hpg->stepgen.instance[i].hal.pin.counts) = 0;
    *(hpg->stepgen.instance[i].hal.pin.position_fb) = 0.0;
    *(hpg->stepgen.instance[i].hal.pin.velocity_fb) = 0.0;
    *(hpg->stepgen.instance[i].hal.pin.enable) = 0;
    *(hpg->stepgen.instance[i].hal.pin.control_type) = 0;

    hpg->stepgen.instance[i].hal.param.position_scale = 1.0;
    hpg->stepgen.instance[i].hal.param.maxvel = 0.0;
    hpg->stepgen.instance[i].hal.param.maxaccel = 1.0;

    hpg->stepgen.instance[i].subcounts = 0;

    hpg->stepgen.instance[i].hal.param.steplen   = (double)DEFAULT_DELAY / (double)hpg->config.pru_period;
    hpg->stepgen.instance[i].hal.param.stepspace = (double)DEFAULT_DELAY / (double)hpg->config.pru_period;
    hpg->stepgen.instance[i].hal.param.dirsetup  = (double)DEFAULT_DELAY / (double)hpg->config.pru_period;
    hpg->stepgen.instance[i].hal.param.dirhold   = (double)DEFAULT_DELAY / (double)hpg->config.pru_period;

    hpg->stepgen.instance[i].written_steplen = 0;
    hpg->stepgen.instance[i].written_stepspace = 0;
    hpg->stepgen.instance[i].written_dirsetup = 0;
    hpg->stepgen.instance[i].written_dirhold = 0;
    hpg->stepgen.instance[i].written_ctrl = 0;

    // Start with 1/2 step offset in accumulator
//    hpg->stepgen.instance[i].PRU.accum = 1 << 26;
    hpg->stepgen.instance[i].PRU.accum = 0;
    hpg->stepgen.instance[i].prev_accumulator = 0;
    hpg->stepgen.instance[i].old_position_cmd = *(hpg->stepgen.instance[i].hal.pin.position_cmd);

    hpg->stepgen.instance[i].hal.param.steppin = PRU_DEFAULT_PIN;
    hpg->stepgen.instance[i].hal.param.dirpin  = PRU_DEFAULT_PIN;

    return 0;
}

int hpg_stepgen_init(hal_pru_generic_t *hpg){
    int r,i;

    if (hpg->config.num_stepgens <= 0)
        return 0;

    // Allocate HAL shared memory for state data
    hpg->stepgen.instance = (hpg_stepgen_instance_t *) hal_malloc(sizeof(hpg_stepgen_instance_t) * hpg->stepgen.num_instances);
    if (hpg->stepgen.instance == 0) {
	    rtapi_print_msg(RTAPI_MSG_ERR,
	        "%s: ERROR: hal_malloc() failed\n", hpg->config.name);
	    hal_exit(hpg->config.comp_id);
	    return -1;
    }

    for (i=0; i < hpg->config.num_stepgens; i++) {
        hpg->stepgen.instance[i].task.addr = pru_malloc(hpg, sizeof(hpg->stepgen.instance[i].PRU));
        hpg->stepgen.instance[i].PRU.task.hdr.mode = eMODE_STEP_DIR;
        pru_task_add(hpg, &(hpg->stepgen.instance[i].task));

        if ((r = export_stepgen(hpg,i)) != 0){ 
            rtapi_print_msg(RTAPI_MSG_ERR,
                    "%s: ERROR: failed to export stepgen %i: %i\n", hpg->config.name,i,r);
            return -1;
        }
    }

    return 0;
}

