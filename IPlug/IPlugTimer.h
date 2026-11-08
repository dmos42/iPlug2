/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/


#pragma once

/** @file
 * @brief This file includes classes for implementing timers - in order to get a regular callback on the main thread
 * The interface is partially based on the api of Steinberg's timer.cpp from the VST3_SDK for compatibility,
 * base/source/timer.cpp, so thanks to them 
 * */

#include <cstring>
#include <stdint.h>
#include <cstring>
#include <cmath>
#include <functional>
#include "ptrlist.h"
#include "mutex.h"

#include "IPlugPlatform.h"

#if defined OS_MAC || defined OS_IOS
#include <CoreFoundation/CoreFoundation.h>
#elif defined OS_WEB
#include <emscripten/html5.h>
#elif defined OS_LINUX
#include <memory>
#include <thread>
#endif

BEGIN_IPLUG_NAMESPACE

/** Base class for timer */
struct Timer
{
  Timer() = default;
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;
  
  using ITimerFunction = std::function<void(Timer& t)>;

  static Timer* Create(ITimerFunction func, uint32_t intervalMs);
  virtual ~Timer() {};
  virtual void Stop() = 0;
};

#if defined OS_MAC || defined OS_IOS

class Timer_impl : public Timer
{
public:
  
  Timer_impl(ITimerFunction func, uint32_t intervalMs);
  ~Timer_impl();
  
  void Stop() override;
  static void TimerProc(CFRunLoopTimerRef timer, void *info);
  
private:
  CFRunLoopTimerRef mOSTimer;
  ITimerFunction mTimerFunc;
  uint32_t mIntervalMs;
};
#elif defined OS_WIN
class Timer_impl : public Timer
{
public:
  Timer_impl(ITimerFunction func, uint32_t intervalMs);
  ~Timer_impl();
  void Stop() override;
  static void CALLBACK TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);
  
private:
  static WDL_Mutex sMutex;
  static WDL_PtrList<Timer_impl> sTimers;
  UINT_PTR ID = 0;
  ITimerFunction mTimerFunc;
  uint32_t mIntervalMs;
};
#elif defined OS_WEB
class Timer_impl : public Timer
{
public:
  Timer_impl(ITimerFunction func, uint32_t intervalMs);
  ~Timer_impl();
  void Stop() override;
  static void TimerProc(void *userData);
  
private:
  long ID = 0;
  ITimerFunction mTimerFunc;
};
#elif defined OS_LINUX
class Timer_impl : public Timer
{
public:
  Timer_impl(ITimerFunction func, uint32_t intervalMs);
  ~Timer_impl() override;
  void Stop() override;

  /** The other platforms deliver the tick on the thread that owns the window, and the
   * plug-in relies on that (OnTimer pushes data into the editor). Linux has no message
   * queue in this layer, so the window implementation registers a dispatcher here and
   * every tick runs inside it. With no dispatcher - no editor open - the tick runs on
   * the timer thread. */
  static void SetThreadDispatcher(std::function<void(std::function<void()>)> dispatcher);

private:
  struct State;
  std::shared_ptr<State> mState;
  std::thread mThread;
};
#else
  #error NOT IMPLEMENTED
#endif

END_IPLUG_NAMESPACE
