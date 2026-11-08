/*
 ==============================================================================
 
 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers. 
 
 See LICENSE.txt for  more info.
 
 ==============================================================================
*/

/**
 * @file
 * @brief Timer implementation
 */

#include "IPlugTimer.h"

using namespace iplug;

#if defined OS_MAC || defined OS_IOS

Timer* Timer::Create(ITimerFunction func, uint32_t intervalMs)
{
  return new Timer_impl(func, intervalMs);
}

Timer_impl::Timer_impl(ITimerFunction func, uint32_t intervalMs)
: mTimerFunc(func)
, mIntervalMs(intervalMs)
{
  CFRunLoopTimerContext context;
  context.version = 0;
  context.info = this;
  context.retain = nullptr;
  context.release = nullptr;
  context.copyDescription = nullptr;
  CFTimeInterval interval = intervalMs / 1000.0;
  CFRunLoopRef runLoop = CFRunLoopGetMain();
  mOSTimer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent(), interval, 0, 0, TimerProc, &context);
  CFRunLoopAddTimer(runLoop, mOSTimer, kCFRunLoopCommonModes);
}

Timer_impl::~Timer_impl()
{
  Stop();
}

void Timer_impl::Stop()
{
  if (mOSTimer)
  {
    CFRunLoopTimerInvalidate(mOSTimer);
    CFRelease(mOSTimer);
    mOSTimer = nullptr;
  }
}

void Timer_impl::TimerProc(CFRunLoopTimerRef timer, void *info)
{
  Timer_impl* itimer = (Timer_impl*) info;
  itimer->mTimerFunc(*itimer);
}

#elif defined OS_WIN

Timer* Timer::Create(ITimerFunction func, uint32_t intervalMs)
{
  return new Timer_impl(func, intervalMs);
}

WDL_Mutex Timer_impl::sMutex;
WDL_PtrList<Timer_impl> Timer_impl::sTimers;

Timer_impl::Timer_impl(ITimerFunction func, uint32_t intervalMs)
: mTimerFunc(func)
, mIntervalMs(intervalMs)

{
  ID = SetTimer(0, 0, intervalMs, TimerProc); //TODO: timer ID correct?
  
  if (ID)
  {
    WDL_MutexLock lock(&sMutex);
    sTimers.Add(this);
  }
}

Timer_impl::~Timer_impl()
{
  Stop();
}

void Timer_impl::Stop()
{
  if (ID)
  {
    KillTimer(0, ID);
    WDL_MutexLock lock(&sMutex);
    sTimers.DeletePtr(this);
    ID = 0;
  }
}

void CALLBACK Timer_impl::TimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
  WDL_MutexLock lock(&sMutex);

  for (auto i = 0; i < sTimers.GetSize(); i++)
  {
    Timer_impl* pTimer = sTimers.Get(i);
    
    if (pTimer->ID == idEvent)
    {
      pTimer->mTimerFunc(*pTimer);
      return;
    }
  }
}
#elif defined OS_WEB
Timer* Timer::Create(ITimerFunction func, uint32_t intervalMs)
{
  return new Timer_impl(func, intervalMs);
}

Timer_impl::Timer_impl(ITimerFunction func, uint32_t intervalMs)
: mTimerFunc(func)
{
  ID = emscripten_set_interval(TimerProc, intervalMs, this);
}

Timer_impl::~Timer_impl()
{
  Stop();
}

void Timer_impl::Stop()
{
  emscripten_clear_interval(ID);
}

void Timer_impl::TimerProc(void* userData)
{
  Timer_impl* itimer = (Timer_impl*) userData;
  itimer->mTimerFunc(*itimer);
}
#elif defined OS_LINUX

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace
{
std::mutex sDispatcherMutex;
std::function<void(std::function<void()>)> sDispatcher;

std::function<void(std::function<void()>)> GetThreadDispatcher()
{
  std::lock_guard<std::mutex> lock(sDispatcherMutex);
  return sDispatcher;
}
} // namespace

/** Held by shared_ptr so a tick that is already queued on the UI thread stays valid if
 * the timer - or the plug-in - goes away before that queue is drained. */
struct Timer_impl::State
{
  std::mutex mMutex;
  std::condition_variable mCv;
  bool mStopRequested = false;
  bool mAlive = true;
  Timer* mTimer = nullptr;
  Timer::ITimerFunction mFunc;
};

Timer* Timer::Create(ITimerFunction func, uint32_t intervalMs)
{
  return new Timer_impl(func, intervalMs);
}

void Timer_impl::SetThreadDispatcher(std::function<void(std::function<void()>)> dispatcher)
{
  std::lock_guard<std::mutex> lock(sDispatcherMutex);
  sDispatcher = std::move(dispatcher);
}

Timer_impl::Timer_impl(ITimerFunction func, uint32_t intervalMs)
: mState(std::make_shared<State>())
{
  mState->mFunc = std::move(func);
  mState->mTimer = this;

  if (intervalMs == 0)
    intervalMs = 1;

  mThread = std::thread([state = mState, intervalMs] {
    for (;;)
    {
      {
        std::unique_lock<std::mutex> lock(state->mMutex);
        state->mCv.wait_for(lock, std::chrono::milliseconds(intervalMs),
                            [state] { return state->mStopRequested; });

        if (state->mStopRequested)
          break;
      }

      auto runTick = [state] {
        std::lock_guard<std::mutex> lock(state->mMutex);

        if (state->mAlive && state->mTimer)
          state->mFunc(*state->mTimer);
      };

      const auto dispatch = GetThreadDispatcher();

      if (dispatch)
        dispatch(runTick);
      else
        runTick();
    }
  });
}

Timer_impl::~Timer_impl()
{
  Stop();
}

void Timer_impl::Stop()
{
  {
    std::lock_guard<std::mutex> lock(mState->mMutex);
    mState->mStopRequested = true;
    mState->mAlive = false;
    mState->mTimer = nullptr;
    mState->mCv.notify_all();
  }

  if (mThread.joinable())
  {
    // A timer stopped from inside its own callback must not wait for itself.
    if (std::this_thread::get_id() == mThread.get_id())
      mThread.detach();
    else
      mThread.join();
  }
}
#endif
