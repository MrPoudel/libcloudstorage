/*****************************************************************************
 * CloudEventLoop.cpp
 *
 *****************************************************************************
 * Copyright (C) 2019 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#include "CloudEventLoop.h"

#include "Utility/Utility.h"

namespace cloudstorage {

CloudEventLoop::CloudEventLoop(IThreadPoolFactory *factory,
                               std::shared_ptr<ICloudFactory::ICallback> cb)
    : callback_(std::move(cb)),
      impl_(std::make_shared<priv::LoopImpl>(factory, this)) {}

CloudEventLoop::~CloudEventLoop() {
  impl_->clear();
  impl_->process_events();
}

void CloudEventLoop::onEventAdded() {
  if (callback_) callback_->onEventsAdded();
}

void CloudEventLoop::processEvents() { impl_->process_events(); }

namespace priv {

LoopImpl::LoopImpl(IThreadPoolFactory *factory, CloudEventLoop *loop)
    : cancellation_thread_pool_(factory->create(1)),
      interrupt_(std::make_shared<std::atomic_bool>(false)),
      event_loop_(loop) {
#ifdef WITH_THUMBNAILER
  thumbnailer_thread_pool_ = factory->create(2);
#endif
}

void LoopImpl::add(uintptr_t tag,
                   const std::shared_ptr<IGenericRequest> &request) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = pending_.find(tag);
  if (it == pending_.end()) {
    pending_.insert({tag, request});
  } else {
    pending_.erase(it);
  }
}

void LoopImpl::fulfill(uintptr_t tag, std::function<void()> &&f) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    auto it = pending_.find(tag);
    if (it != pending_.end()) {
      auto request = std::move(it->second);
      pending_.erase(it);
      lock.unlock();
      invoke([request = std::move(request)] { request->finish(); });
    } else {
      pending_.insert({tag, nullptr});
    }
  }
  invoke(std::move(f));
}

void LoopImpl::cancel(uintptr_t tag) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto it = pending_.find(tag);
  if (it != pending_.end()) {
    auto request = std::move(it->second);
    pending_.erase(it);
    if (cancellation_thread_pool_) {
      lock.unlock();
      cancellation_thread_pool_->schedule(
          [request = std::move(request)] { request->cancel(); });
    } else {
      lock.unlock();
      request->cancel();
    }
  }
}

void LoopImpl::invoke(std::function<void()> &&f) {
  {
    std::unique_lock<std::mutex> lock(mutex_);
    events_.emplace_back(std::move(f));
  }
  event_loop_->onEventAdded();
}

#ifdef WITH_THUMBNAILER
void LoopImpl::invokeOnThreadPool(std::function<void()> &&f) {
  std::unique_lock<std::mutex> lock(thumbnailer_mutex_);
  if (thumbnailer_thread_pool_) {
    thumbnailer_thread_pool_->schedule(f);
  } else {
    lock.unlock();
    f();
  }
}

std::shared_ptr<IThreadPool> LoopImpl::thumbnailer_thread_pool() const {
  return thumbnailer_thread_pool_;
}
#endif

void LoopImpl::process_events() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (const auto &e : events_) {
    lock.unlock();
    e();
    lock.lock();
  }
  events_.clear();
}

void LoopImpl::clear() {
  *interrupt_ = true;
#ifdef WITH_THUMBNAILER
  {
    std::unique_lock<std::mutex> lock(thumbnailer_mutex_);
    thumbnailer_thread_pool_ = nullptr;
  }
#endif
  cancellation_thread_pool_ = nullptr;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!pending_.empty()) {
      auto it = pending_.begin();
      auto request = std::move(it->second);
      pending_.erase(it);
      if (request) {
        lock.unlock();
        request->cancel();
        lock.lock();
      }
      lock.unlock();
      process_events();
      lock.lock();
    }
  }
}

}  // namespace priv

Exception::Exception(int code, std::string description)
    : code_(code), description_(std::move(description)) {}

Exception::Exception(const std::shared_ptr<Error> &e)
    : Exception(e->code_, e->description_) {}

// namespace priv

}  // namespace cloudstorage
