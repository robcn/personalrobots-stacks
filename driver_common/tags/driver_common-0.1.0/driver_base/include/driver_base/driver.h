/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

// Author: Blaise Gassend
#ifndef __DRIVER_BASE__DRIVER_H__
#define __DRIVER_BASE__DRIVER_H__

namespace driver_base
{

/**
 *
 * State transition functions assume that they are called from the correct
 * state. It is the caller's responsibility to check that the state is
 * correct. The Driver mutex_ should always be locked by the caller before
 * checking the current state and throughout the call to the transition function.
 *
 * State should not change between calls to the transition function except
 * for transitions from RUNNING to STOPPED. The device must hold its lock
 * while doing this transition to allow the caller to enforce that stop is
 * only called from the RUNNING state.
 *
 * Transitions may fail. It is the caller's responsibility to check success
 * by looking at the device's state_. After a failure, the device can set
 * itself into any state. For example, if a call to start() fails, the
 * device may end up in the CLOSED state.
 *
 */

class Driver
{
public:  
  typedef char state_t;

protected:
  state_t state_;

  virtual void doOpen() = 0;
  virtual void doClose() = 0;
  virtual void doStart() = 0;
  virtual void doStop() = 0;

  typedef boost::function< void() > hookFunction;
  hookFunction postOpenHook;

public:
  void setPostOpenHook(hookFunction f)
  {
    postOpenHook = f;
  }

  virtual std::string getID() = 0;
  
  boost::mutex mutex_; ///@todo should this be protected?

  static const state_t CLOSED = 0; // Not connected to the hardware.
  static const state_t OPENED = 1; // Connected to the hardware, ready to start streaming.
  static const state_t RUNNING = 2; // Streaming data.

  bool goState(state_t target)
  {
    boost::mutex::scoped_lock(mutex_);

    if (state_ > target)
      return lowerState(target);

    if (state_ < target)
      return raiseState(target);

    return true;
  }

  bool raiseState(state_t target)
  {
    boost::mutex::scoped_lock(mutex_);

    switch (getState())  
    {
      case Driver::CLOSED:
        if (target <= Driver::CLOSED)
          return true;
        if (!tryTransition(Driver::OPENED, &Driver::doOpen))
          return false;

      case Driver::OPENED:
        if (target <= Driver::OPENED)
          return true;
        if (!tryTransition(Driver::RUNNING, &Driver::doStart))
          return false;

      default:
        return target <= getState();
    }
  }

  bool lowerState(state_t target)
  {
    boost::mutex::scoped_lock(mutex_);

    switch (getState())  
    {
      case Driver::RUNNING:
        if (target >= Driver::RUNNING)
          return true;
        if (!tryTransition(Driver::OPENED, &Driver::doStop))
          return false;

      case Driver::OPENED:
        if (target >= Driver::OPENED)
          return true;
        if (!tryTransition(Driver::CLOSED, &Driver::doClose))
          return false;

      default:
        return target >= getState();
    }
  }

  bool goRunning()
  {
    return goState(Driver::RUNNING);
  }

  bool goOpened()
  {
    return goState(Driver::OPENED);
  }

  bool goClosed()
  {
    return goState(Driver::CLOSED);
  }

  bool stop()
  {
    return lowerState(Driver::OPENED);
  }

  bool start()
  {
    return raiseState(Driver::RUNNING);
  }
  
  bool open()
  {
    return raiseState(Driver::OPENED);
  }
  
  bool close()
  {
    return lowerState(Driver::CLOSED);
  }
  
  bool isRunning()
  {
    return getState() == Driver::RUNNING;
  }

  bool isOpened()
  {
    return getState() == Driver::OPENED;
  }

  bool isClosed()
  {
    return getState() == Driver::CLOSED;
  }
  
  bool isStopped()
  {
    state_t s = getState();
    return s == Driver::CLOSED || s == Driver::OPENED;
  }

  state_t getState()
  {
    return state_;
  }

  const std::string getStateName()
  {
    return getStateName(state_);
  }
  
  static const std::string &getStateName(state_t s)
  {
    static const std::string names[4] = {
      std::string("CLOSED"),
      std::string("OPENED"),
      std::string("RUNNING"),
      std::string("Unknown")
    };
  
    if (s >= 0 && s <= 2)
      return names[(int) s];
    else
      return names[3];
  }

  Driver() : state_(CLOSED) {}
  virtual ~Driver() {}

private:  
  const std::string &getTransitionName(void (Driver::*transition)())
  {
    static const std::string open = std::string("open");
    static const std::string close = std::string("close");
    static const std::string start = std::string("start");
    static const std::string stop = std::string("stop");
    static const std::string unknown = std::string("Unknown");

    if (transition == &Driver::doOpen)
      return open;
    if (transition == &Driver::doClose)
      return close;
    if (transition == &Driver::doStart)
      return start;
    if (transition == &Driver::doStop)
      return stop;
    
    return unknown;
  }

  bool tryTransition(state_t target, void (Driver::*transition)())
  {
    state_t orig = state_;
    ROS_DEBUG("Trying transition %s from %s to %s.", getTransitionName(transition).c_str(), getStateName(orig).c_str(), getStateName(target).c_str());
    boost::mutex::scoped_lock(mutex_);
    (this->*transition)();
    bool out = state_ == target;
    ROS_DEBUG("Transition %s from %s to %s %s.", getTransitionName(transition).c_str(), getStateName(orig).c_str(), getStateName(target).c_str(), out ? "succeeded" : "failed");
    if (out && transition == &Driver::doOpen)
      postOpenHook();
    return out;
  }
};

/// @fixme derived classes for nodes that only poll or only stream.

};

#endif

