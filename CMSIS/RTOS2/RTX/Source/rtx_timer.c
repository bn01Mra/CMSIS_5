/*
 * Copyright (c) 2013-2016 ARM Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * -----------------------------------------------------------------------------
 *
 * Project:     CMSIS-RTOS RTX
 * Title:       Timer functions
 *
 * -----------------------------------------------------------------------------
 */

#include "rtx_lib.h"


//  ==== Helper functions ====

/// Insert Timer into the Timer List sorted by Time.
/// \param[in]  timer           timer object.
/// \param[in]  tick            timer tick.
static void os_TimerInsert (os_timer_t *timer, uint32_t tick) {
  os_timer_t *prev, *next;

  prev = NULL;
  next = os_Info.timer.list;
  while ((next != NULL) && (next->tick <= tick)) {
    tick -= next->tick;
    prev  = next;
    next  = next->next;
  }
  timer->tick = tick;
  timer->prev = prev;
  timer->next = next;
  if (next != NULL) {
    next->tick -= timer->tick;
    next->prev  = timer;
  }
  if (prev != NULL) {
    prev->next = timer;
  } else {
    os_Info.timer.list = timer;
  }
}

/// Remove Timer from the Timer List.
/// \param[in]  timer           timer object.
static void os_TimerRemove (os_timer_t *timer) {

  if (timer->next != NULL) {
    timer->next->tick += timer->tick;
    timer->next->prev  = timer->prev;
  }
  if (timer->prev != NULL) {
    timer->prev->next  = timer->next;
  } else {
    os_Info.timer.list = timer->next;
  }
}

/// Unlink Timer from the Timer List Head.
/// \param[in]  timer           timer object.
static void os_TimerUnlink (os_timer_t *timer) {

  if (timer->next != NULL) {
    timer->next->prev = timer->prev;
  }
  os_Info.timer.list = timer->next;
}


//  ==== Library functions ====

/// Timer Tick (called each SysTick).
void os_TimerTick (void) {
  os_timer_t *timer;
  osStatus_t  status;

  timer = os_Info.timer.list;
  if (timer == NULL) {
    return;
  }

  timer->tick--;
  while ((timer != NULL) && (timer->tick == 0U)) {
    os_TimerUnlink(timer);
    status = osMessageQueuePut((osMessageQueueId_t)os_Info.timer.mq, &timer->finfo, 0U, 0U);
    if (status != osOK) {
      os_Error(os_ErrorTimerQueueOverflow, timer);
    }
    if (timer->type == os_TimerPeriodic) {
      os_TimerInsert(timer, timer->load);
    } else {
      timer->state = os_TimerStopped;
    }
    timer = os_Info.timer.list;
  }
}

/// Timer Thread
void *os_TimerThread (void *argument) {
  os_timer_finfo_t finfo;
  osStatus_t       status;
  (void)           argument;

  for (;;) {
    status = osMessageQueueGet((osMessageQueueId_t)os_Info.timer.mq, &finfo, NULL, osWaitForever);
    if (status == osOK) {
      (*(os_timer_func_t)finfo.fp)(finfo.arg);
    }
  }
  
  return NULL;
}

//  ==== Service Calls ====

//  Service Calls definitions
SVC0_4(TimerNew,       osTimerId_t, os_timer_func_t, osTimerType_t, void *, const osTimerAttr_t *)
SVC0_2(TimerStart,     osStatus_t,  osTimerId_t, uint32_t)
SVC0_1(TimerStop,      osStatus_t,  osTimerId_t)
SVC0_1(TimerIsRunning, uint32_t,    osTimerId_t)
SVC0_1(TimerDelete,    osStatus_t,  osTimerId_t)

/// Create and Initialize a timer.
/// \note API identical to osTimerNew
osTimerId_t os_svcTimerNew (os_timer_func_t func, osTimerType_t type, void *argument, const osTimerAttr_t *attr) {
  os_timer_t *timer;
  uint8_t     flags;
  const char *name;

  // Create common timer message queue if not yet active
  if (os_Info.timer.mq == NULL) {
    os_Info.timer.mq = (os_message_queue_t *)(os_svcMessageQueueNew(
                                                os_Config.timer_mq_mcnt,
                                                sizeof(os_timer_finfo_t),
                                                os_Config.timer_mq_attr));
    if (os_Info.timer.mq == NULL) {
      return (osTimerId_t)NULL;
    }
  }

  // Create common timer thread if not yet active
  if (os_Info.timer.thread == NULL) {
    os_Info.timer.thread = (os_thread_t *)(os_svcThreadNew(
                                             os_TimerThread,
                                             NULL,
                                             os_Config.timer_thread_attr));
    if (os_Info.timer.thread == NULL) {
      return (osTimerId_t)NULL;
    }
  }

  // Check parameters
  if ((func == NULL) || ((type != osTimerOnce) && (type != osTimerPeriodic))) {
    return (osTimerId_t)NULL;
  }

  // Check timer objects
  if ((os_Info.timer.thread == NULL) || (os_Info.timer.mq == NULL)) {
    return (osTimerId_t)NULL;
  }

  // Process attributes
  if (attr != NULL) {
    name  = attr->name;
    timer = attr->cb_mem;
    if (timer != NULL) {
      if (((uint32_t)timer & 3U) || (attr->cb_size < sizeof(os_timer_t))) {
        return (osTimerId_t)NULL;
      }
    } else {
      if (attr->cb_size != 0U) {
        return (osTimerId_t)NULL;
      }
    }
  } else {
    name  = NULL;
    timer = NULL;
  }

  // Allocate object memory if not provided
  if (timer == NULL) {
    if (os_Info.mpi.timer != NULL) {
      timer = os_MemoryPoolAlloc(os_Info.mpi.timer);
    } else {
      timer = os_MemoryAlloc(os_Info.mem.cb, sizeof(os_timer_t));
    }
    if (timer == NULL) {
      return (osTimerId_t)NULL;
    }
    flags = os_FlagSystemObject;
  } else {
    flags = 0U;
  }

  // Initialize control block
  timer->id        = os_IdTimer;
  timer->state     = os_TimerStopped;
  timer->flags     = flags;
  timer->type      = (uint8_t)type;
  timer->name      = name;
  timer->prev      = NULL;
  timer->next      = NULL;
  timer->tick      = 0U;
  timer->load      = 0U;
  timer->finfo.fp  = (void *)func;
  timer->finfo.arg = argument;

  return (osTimerId_t)timer;
}

/// Start or restart a timer.
/// \note API identical to osTimerStart
osStatus_t os_svcTimerStart (osTimerId_t timer_id, uint32_t millisec) {
  os_timer_t *timer = (os_timer_t *)timer_id;

  // Check parameters
  if ((timer == NULL) ||
      (timer->id != os_IdTimer)) {
    return osErrorParameter;
  }
  if (millisec == 0U) {
    return osErrorParameter;
  }

  // Check object state
  switch (timer->state) {
    case os_TimerStopped:
      timer->state = os_TimerRunning;
      timer->load  = millisec;
      break;
    case os_TimerRunning:
      os_TimerRemove(timer);
      break;
    case os_TimerInactive:
    default:
      return osErrorResource;
  }

  os_TimerInsert(timer, millisec);

  return osOK;
}

/// Stop a timer.
/// \note API identical to osTimerStop
osStatus_t os_svcTimerStop (osTimerId_t timer_id) {
  os_timer_t *timer = (os_timer_t *)timer_id;

  // Check parameters
  if ((timer == NULL) ||
      (timer->id != os_IdTimer)) {
    return osErrorParameter;
  }

  // Check object state
  if (timer->state != os_TimerRunning) {
    return osErrorResource;
  }

  timer->state = os_TimerStopped;

  os_TimerRemove(timer);

  return osOK;
}

/// Check if a timer is running.
/// \note API identical to osTimerIsRunning
uint32_t os_svcTimerIsRunning (osTimerId_t timer_id) {
  os_timer_t *timer = (os_timer_t *)timer_id;

  // Check parameters
  if ((timer == NULL) ||
      (timer->id != os_IdTimer)) {
    return 0U;
  }

  // Check object state
  if (timer->state == os_TimerRunning) {
    return 1U;
  }

  return 0U;
}

/// Delete a timer.
/// \note API identical to osTimerDelete
osStatus_t os_svcTimerDelete (osTimerId_t timer_id) {
  os_timer_t *timer = (os_timer_t *)timer_id;

  // Check parameters
  if ((timer == NULL) ||
      (timer->id != os_IdTimer)) {
    return osErrorParameter;
  }

  // Check object state
  switch (timer->state) {
    case os_TimerStopped:
      break;
    case os_TimerRunning:
      os_TimerRemove(timer);
      break;
    case os_TimerInactive:
    default:
      return osErrorResource;
  }

  // Mark object as inactive
  timer->state = os_TimerInactive;

  // Free object memory
  if (timer->flags & os_FlagSystemObject) {
    if (os_Info.mpi.timer != NULL) {
      os_MemoryPoolFree(os_Info.mpi.timer, timer);
    } else {
      os_MemoryFree(os_Info.mem.cb, timer);
    }
  }

  return osOK;
}


//  ==== Public API ====

/// Create and Initialize a timer.
osTimerId_t osTimerNew (os_timer_func_t func, osTimerType_t type, void *argument, const osTimerAttr_t *attr) {
  if (__get_IPSR() != 0U) {
    return (osTimerId_t)NULL;                   // Not allowed in ISR
  }
  if ((os_KernelGetState() == os_KernelReady) && ((__get_CONTROL() & 1U) == 0U)) {
    // Kernel Ready (not running) and in Priviledged mode
    return os_svcTimerNew(func, type, argument, attr);
  } else {
    return  __svcTimerNew(func, type, argument, attr);
  }
}

/// Start or restart a timer.
osStatus_t osTimerStart (osTimerId_t timer_id, uint32_t millisec) {
  if (__get_IPSR() != 0U) {
    return osErrorISR;                          // Not allowed in ISR
  }
  return __svcTimerStart(timer_id, millisec);
}

/// Stop a timer.
osStatus_t osTimerStop (osTimerId_t timer_id) {
  if (__get_IPSR() != 0U) {
    return osErrorISR;                          // Not allowed in ISR
  }
  return __svcTimerStop(timer_id);
}

/// Check if a timer is running.
uint32_t osTimerIsRunning (osTimerId_t timer_id) {
  if (__get_IPSR() != 0U) {
    return 0U;                                  // Not allowed in ISR
  }
  return __svcTimerIsRunning(timer_id);
}

/// Delete a timer.
osStatus_t osTimerDelete (osTimerId_t timer_id) {
  if (__get_IPSR() != 0U) {
    return osErrorISR;                          // Not allowed in ISR
  }
  return __svcTimerDelete(timer_id);
}
