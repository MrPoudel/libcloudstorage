/*****************************************************************************
 * FileServer.cpp
 *
 *****************************************************************************
 * Copyright (C) 2018 VideoLAN
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
#include "FileServer.h"

#include <json/json.h>

#include <algorithm>
#include <queue>

#include "Utility/Item.h"

namespace cloudstorage {

const int CHUNK_SIZE = 8 * 1024 * 1024;
const int CACHE_SIZE = 128;

namespace {

struct Buffer;
using Cache = util::LRUCache<std::string, IItem>;

class HttpServerCallback : public IHttpServer::ICallback {
 public:
  HttpServerCallback(std::shared_ptr<CloudProvider>);
  IHttpServer::IResponse::Pointer handle(const IHttpServer::IRequest&) override;

 private:
  std::shared_ptr<Cache> item_cache_;
  std::shared_ptr<CloudProvider> provider_;
};

class HttpDataCallback : public IDownloadFileCallback {
 public:
  HttpDataCallback(std::shared_ptr<Buffer> d) : buffer_(std::move(d)) {}

  void receivedData(const char* data, uint32_t length) override;
  void done(EitherError<void> e) override;
  void progress(uint64_t, uint64_t) override {}

  std::shared_ptr<Buffer> buffer_;
};

class StreamRequest : public Request<EitherError<void>> {
 public:
  using Request::Request;

  void cancel() override {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!done_called_) {
        done_called_ = true;
        lock.unlock();
        Request::done(Error{IHttpRequest::Aborted, util::Error::ABORTED});
      }
    }
    Request::cancel();
  }

  void done(const EitherError<void>& e) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!done_called_) {
      done_called_ = true;
      lock.unlock();
      try {
        Request::done(e);
      } catch (const std::runtime_error&) {
      }
    }
  }

 private:
  bool done_called_ = false;
  std::mutex mutex_;
};

struct Buffer : public std::enable_shared_from_this<Buffer> {
  using Pointer = std::shared_ptr<Buffer>;

  int read(char* buf, uint32_t max) {
    if (2 * size() < CHUNK_SIZE) {
      std::unique_lock<std::mutex> lock(delayed_mutex_);
      if (delayed_) {
        delayed_ = false;
        lock.unlock();
        run_download();
      }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (abort_) return IHttpServer::IResponse::ICallback::Abort;
    if (data_.empty()) return IHttpServer::IResponse::ICallback::Suspend;
    size_t cnt = std::min<size_t>(data_.size(), static_cast<size_t>(max));
    for (size_t i = 0; i < cnt; i++) {
      buf[i] = data_.front();
      data_.pop();
    }
    return static_cast<int>(cnt);
  }

  void put(const char* data, uint32_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t i = 0; i < length; i++) data_.push(data[i]);
  }

  void done(const EitherError<void>& e) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (e.left()) {
      abort_ = true;
      if (done_) return;
      done_ = true;
      lock.unlock();
      if (e.left()->code_ != IHttpRequest::Aborted)
        util::log("[HTTP SERVER] download failed", e.left()->code_,
                  e.left()->description_);
      request_->done(e);
    }
  }

  void resume() {
    std::lock_guard<std::mutex> lock(response_mutex_);
    if (response_) response_->resume();
  }

  std::size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.size();
  }

  void continue_download(const EitherError<void>& e) {
    if (e.left() || range_.size_ < CHUNK_SIZE) return done(e);
    range_.size_ -= CHUNK_SIZE;
    range_.start_ += CHUNK_SIZE;
    if (2 * size() < CHUNK_SIZE)
      run_download();
    else {
      std::unique_lock<std::mutex> lock(delayed_mutex_);
      delayed_ = true;
    }
  }

  void run_download() {
    request_->make_subrequest(
        &CloudProvider::downloadFileRangeAsync, item_,
        Range{range_.start_, std::min<uint64_t>(range_.size_, CHUNK_SIZE)},
        util::make_unique<HttpDataCallback>(shared_from_this()));
  }

  std::mutex mutex_;
  std::queue<char> data_;
  std::mutex response_mutex_;
  IHttpServer::IResponse* response_;
  std::shared_ptr<StreamRequest> request_;
  IItem::Pointer item_;
  Range range_;
  std::mutex delayed_mutex_;
  bool delayed_ = false;
  bool done_ = false;
  bool abort_ = false;
};

void HttpDataCallback::receivedData(const char* data, uint32_t length) {
  buffer_->put(data, length);
  buffer_->resume();
}

void HttpDataCallback::done(EitherError<void> e) {
  buffer_->resume();
  buffer_->continue_download(e);
}

class HttpData : public IHttpServer::IResponse::ICallback {
 public:
  static constexpr int InProgress = 0;
  static constexpr int Success = 1;
  static constexpr int Failed = 2;

  HttpData(Buffer::Pointer d, const std::shared_ptr<CloudProvider>& p,
           const std::string& file, Range range,
           const std::shared_ptr<Cache>& cache)
      : status_(InProgress),
        buffer_(std::move(d)),
        provider_(p),
        request_(request(p, file, range, cache)) {}

  ~HttpData() override {
    buffer_->done(Error{IHttpRequest::Aborted, util::Error::ABORTED});
    provider_->removeStreamRequest(request_);
  }

  std::shared_ptr<ICloudProvider::DownloadFileRequest> request(
      std::shared_ptr<CloudProvider> provider, const std::string& file,
      Range range, const std::shared_ptr<Cache>& cache) {
    auto resolver = [=, this](Request<EitherError<void>>::Pointer r) {
      buffer_->request_ = std::static_pointer_cast<StreamRequest>(r);
      auto item_received = [=, this](EitherError<IItem> e) {
        if (e.left()) {
          status_ = Failed;
          util::log("[HTTP SERVER] couldn't get item", e.left()->code_,
                    e.left()->description_);
          buffer_->done(Error{IHttpRequest::Bad, util::Error::INVALID_NODE});
        } else {
          if (range.start_ + range.size_ > uint64_t(e.right()->size())) {
            status_ = Failed;
            util::log("[HTTP SERVER] invalid range", range.start_, range.size_);
            buffer_->done(Error{IHttpRequest::Bad, util::Error::INVALID_RANGE});
          } else {
            status_ = Success;
            buffer_->item_ = e.right();
            buffer_->range_ = range;
            cache->put(file, e.right());
            r->make_subrequest(
                &CloudProvider::downloadFileRangeAsync, e.right(),
                Range{range.start_,
                      std::min<uint64_t>(range.size_, CHUNK_SIZE)},
                util::make_unique<HttpDataCallback>(buffer_));
          }
        }
        buffer_->resume();
      };
      auto cached_item = cache->get(file);
      if (cached_item == nullptr)
        r->make_subrequest(&CloudProvider::getItemDataAsync, file,
                           item_received);
      else
        item_received(cached_item);
    };
    auto result = std::make_shared<StreamRequest>(
        provider,
        [=, this](EitherError<void> e) {
          if (e.left()) status_ = Failed;
          buffer_->resume();
        },
        resolver);
    provider->addStreamRequest(result);
    return result->run();
  }

  int putData(char* buf, size_t max) override {
    if (status_ == Failed)
      return Abort;
    else if (status_ == InProgress)
      return Suspend;
    else
      return buffer_->read(buf, static_cast<uint32_t>(max));
  }

  std::atomic_int status_;
  Buffer::Pointer buffer_;
  std::shared_ptr<CloudProvider> provider_;
  std::shared_ptr<ICloudProvider::DownloadFileRequest> request_;
};

HttpServerCallback::HttpServerCallback(std::shared_ptr<CloudProvider> p)
    : item_cache_(util::make_unique<Cache>(CACHE_SIZE)),
      provider_(std::move(p)) {}

IHttpServer::IResponse::Pointer HttpServerCallback::handle(
    const IHttpServer::IRequest& request) {
  try {
    auto url_fragment =
        std::string(request.url())
            .substr(std::string(request.url()).find_last_of('/') + 1);
    std::replace(url_fragment.begin(), url_fragment.end(), '-', '/');
    auto json = util::json::from_string(util::from_base64(url_fragment));
    if (json["state"] != provider_->auth()->state())
      return util::response_from_string(request, IHttpRequest::Bad, {},
                                        util::Error::INVALID_STATE);
    auto id = json["id"].asString();
    auto filename = json["name"].asString();
    auto size = json["size"].asUInt64();
    auto extension = filename.substr(filename.find_last_of('.') + 1);
    std::unordered_map<std::string, std::string> headers = {
        {"Content-Type", util::to_mime_type(extension)},
        {"Accept-Ranges", "bytes"},
        {"Content-Disposition", "inline; filename=\"" + filename + "\""},
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Headers", "*"}};
    if (request.method() == "OPTIONS")
      return util::response_from_string(request, IHttpRequest::Ok, headers, "");
    Range range = {0, size};
    int code = IHttpRequest::Ok;
    if (const char* range_str = request.header("Range")) {
      range = util::parse_range(range_str);
      if (range.size_ == Range::Full) range.size_ = size - range.start_;
      if (range.start_ + range.size_ > size)
        return util::response_from_string(request, IHttpRequest::RangeInvalid,
                                          {}, util::Error::INVALID_RANGE);
      std::stringstream stream;
      stream << "bytes " << range.start_ << "-"
             << range.start_ + range.size_ - 1 << "/" << size;
      headers["Content-Range"] = stream.str();
      code = IHttpRequest::Partial;
    }
    auto buffer = std::make_shared<Buffer>();
    auto data =
        util::make_unique<HttpData>(buffer, provider_, id, range, item_cache_);
    auto response =
        request.response(code, headers, range.size_, std::move(data));
    buffer->response_ = response.get();
    response->completed([buffer]() {
      std::unique_lock<std::mutex> lock(buffer->response_mutex_);
      buffer->response_ = nullptr;
    });
    return response;
  } catch (const Json::Exception& e) {
    util::log("[HTTP SERVER] invalid request", request.url(), e.what());
    return util::response_from_string(request, IHttpRequest::Bad, {},
                                      util::Error::INVALID_REQUEST);
  }
}

}  // namespace

IHttpServer::Pointer FileServer::create(std::shared_ptr<CloudProvider> p,
                                        const std::string& session) {
  return p->http_server()->create(util::make_unique<HttpServerCallback>(p),
                                  session, IHttpServer::Type::FileProvider);
}
}  // namespace cloudstorage
