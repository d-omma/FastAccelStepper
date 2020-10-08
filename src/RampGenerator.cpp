#include <stdint.h>

#include "FastAccelStepper.h"
#include "StepperISR.h"
#include "RampGenerator.h"

#ifdef TEST
#include <assert.h>
#endif

// This define in order to not shoot myself.
#ifndef TEST
#define printf DO_NOT_USE_PRINTF
#endif

void RampGenerator::single_fill_queue(const struct ramp_ro_s *ro, struct ramp_rw_s *rw,FastAccelStepper *stepper, uint32_t ticks_at_queue_end, int32_t position_at_queue_end) {
#if (TEST_MEASURE_ISR_SINGLE_FILL == 1)
  // For run time measurement
  uint32_t runtime_us = micros();
#endif

  if (ticks_at_queue_end == 0) {
    ticks_at_queue_end = TICKS_FOR_STOPPED_MOTOR;
  }

  // check state for acceleration/deceleration or deceleration to stop
  uint32_t remaining_steps = abs(ro->target_pos - position_at_queue_end);
  uint32_t planning_steps;
  uint32_t next_ticks;
  if (rw->ramp_state == RAMP_STATE_IDLE) {  // motor is stopped. Set to max value
    planning_steps = 1;
    rw->ramp_state = RAMP_STATE_ACCELERATE;
  } else {
    // TODO:explain the 16000
    planning_steps = max(16000 / ticks_at_queue_end, 1);
    if (remaining_steps <= ro->deceleration_start) {
      rw->ramp_state = RAMP_STATE_DECELERATE_TO_STOP;
    } else if (ro->min_travel_ticks < ticks_at_queue_end) {
      rw->ramp_state = RAMP_STATE_ACCELERATE;
    } else if (ro->min_travel_ticks > ticks_at_queue_end) {
      rw->ramp_state = RAMP_STATE_DECELERATE;
    } else {
      rw->ramp_state = RAMP_STATE_COAST;
    }
  }

  // Forward planning of minimum 10ms or more on slow speed.

#ifdef TEST
  printf(
      "pos@queue_end=%d remaining=%u deceleration_start=%u planning steps=%d  "
      " ",
      position_at_queue_end, remaining_steps, ro->deceleration_start,
      planning_steps);
  switch (rw->ramp_state) {
    case RAMP_STATE_COAST:
      printf("COAST");
      break;
    case RAMP_STATE_ACCELERATE:
      printf("ACC");
      break;
    case RAMP_STATE_DECELERATE:
      printf("DEC");
      break;
    case RAMP_STATE_DECELERATE_TO_STOP:
      printf("STOP");
      break;
  }
  printf("\n");
#endif

  uint32_t curr_ticks = ticks_at_queue_end;
#ifdef TEST
  float v2;
#endif
  switch (rw->ramp_state) {
    uint32_t d_ticks_new;
    uint32_t upm_rem_steps;
    upm_float upm_d_ticks_new;
    case RAMP_STATE_COAST:
      next_ticks = ro->min_travel_ticks;
      // do not overshoot ramp down start
      planning_steps =
          min(planning_steps, remaining_steps - ro->deceleration_start);
      break;
    case RAMP_STATE_ACCELERATE:
      upm_rem_steps = upm_from(rw->performed_ramp_up_steps + planning_steps);
      upm_d_ticks_new = sqrt(divide(ro->upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid overshoot
      next_ticks = max(d_ticks_new, ro->min_travel_ticks);
      if (rw->performed_ramp_up_steps == 0) {
        curr_ticks = d_ticks_new;
      } else {
        // CLIPPING: avoid increase
        next_ticks = min(next_ticks, curr_ticks);
      }

#ifdef TEST
      printf("accelerate ticks => %d  during %d ticks (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
      printf("... %u+%u steps\n", rw->performed_ramp_up_steps, planning_steps);
#endif
      break;
    case RAMP_STATE_DECELERATE:
      upm_rem_steps = upm_from(rw->performed_ramp_up_steps + planning_steps);
      upm_d_ticks_new = sqrt(divide(ro->upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid undershoot
      next_ticks = min(d_ticks_new, ro->min_travel_ticks);

      // CLIPPING: avoid reduction
      next_ticks = max(next_ticks, curr_ticks);

#ifdef TEST
      printf("decelerate ticks => %d  during %d ticks (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
      printf("... %u+%u steps\n", rw->performed_ramp_up_steps, planning_steps);
#endif
      break;
    case RAMP_STATE_DECELERATE_TO_STOP:
      upm_rem_steps = upm_from(remaining_steps - planning_steps);
      upm_d_ticks_new = sqrt(divide(ro->upm_inv_accel2, upm_rem_steps));

      d_ticks_new = upm_to_u32(upm_d_ticks_new);

      // avoid undershoot
      next_ticks = max(d_ticks_new, ro->min_travel_ticks);

      // CLIPPING: avoid reduction
      next_ticks = max(next_ticks, curr_ticks);
#ifdef TEST
      printf("decelerate ticks => %d  during %d ticks (d_ticks_new = %u)\n",
             next_ticks, planning_steps, d_ticks_new);
#endif
      break;
    default:
      // TODO: how to treat this (error) case ?
      next_ticks = curr_ticks;
#ifdef TEST
      assert(false);
#endif
  }

  // CLIPPING: avoid increase
  next_ticks = min(next_ticks, ABSOLUTE_MAX_TICKS);

  // Number of steps to execute with limitation to min 1 and max remaining steps
  uint16_t total_steps = planning_steps;
#ifdef TEST
  printf(
      "total_steps for the command = %d  with planning_steps = %u and "
      "next_ticks = %u\n",
      total_steps, planning_steps, next_ticks);
#endif
  total_steps = max(total_steps, 1);
  total_steps = min(total_steps, abs(remaining_steps));
  uint16_t steps = total_steps;

  // Calculate change per step
  int32_t total_change = (int32_t)next_ticks - (int32_t)curr_ticks;

  // Number of commands
  uint8_t command_cnt = steps / 128 + 1;

  // Steps per command
  uint16_t steps_per_command = (steps + command_cnt - 1) / command_cnt;

  if (steps_per_command * command_cnt > steps) {
    steps_per_command -= 1;
  }

  int32_t change_per_command = total_change / command_cnt;

  if (rw->ramp_state == RAMP_STATE_ACCELERATE) {
    rw->performed_ramp_up_steps += steps;
  }
  if (rw->ramp_state == RAMP_STATE_DECELERATE) {
    rw->performed_ramp_up_steps -= steps;
  }
  // Apply change to curr_ticks
  if (command_cnt > 1) {
    curr_ticks += change_per_command / 2;
  } else {
    curr_ticks = next_ticks;
  }

  bool dir = (ro->target_pos > position_at_queue_end) == ro->dirHighCountsUp;

#ifdef TEST
  if (command_cnt > 1) {
    printf(
        "Issue %d commands for %d steps with %d steps/command for total "
        "change %d and %d change/command and start with %u ticks\n",
        command_cnt, steps, steps_per_command, total_change, change_per_command,
        curr_ticks);
  } else {
    printf(
        "Issue %d command for %d steps "
        "and start with %u ticks\n",
        command_cnt, steps, curr_ticks);
  }
  assert(curr_ticks > 0);
#endif

  for (uint16_t c = 0; c < command_cnt; c++) {
    int8_t res = stepper->addQueueEntry(curr_ticks, steps_per_command, dir);
#ifdef TEST
    printf(
        "add command %d Steps = %d start_ticks = %d  Target "
        "pos = %d "
        "Remaining steps = %d  tick_change=%d"
        " => res=%d",
        (command_cnt + 1 - c), steps_per_command, curr_ticks, ro->target_pos,
        remaining_steps, change_per_command, res);
#endif
    if (res != 0) {
      if (res == AQE_FULL) {
        return;
      }
      // Emergency stop on internal error
      rw->ramp_state = RAMP_STATE_IDLE;
      rw->speed_control_enabled = false;
      return;
    }
    steps -= steps_per_command;
    curr_ticks += change_per_command;
  }
  if (total_steps == abs(remaining_steps)) {
    rw->ramp_state = RAMP_STATE_IDLE;
    rw->speed_control_enabled = false;
#ifdef TEST
    puts("Stepper stop");
#endif
  }

#ifdef TEST
  puts("");
#endif

#if (TEST_MEASURE_ISR_SINGLE_FILL == 1)
  // For run time measurement
  runtime_us = micros() - runtime_us;
  max_micros = max(max_micros, runtime_us);
#endif
}