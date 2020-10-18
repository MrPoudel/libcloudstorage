/*****************************************************************************
 * GenerateThumbnail.cpp
 *
 *****************************************************************************
 * Copyright (C) 2016 VideoLAN
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

#ifdef WITH_THUMBNAILER

#include "GenerateThumbnail.h"
#include "IHttp.h"
#include "IRequest.h"
#include "Utility/Utility.h"

#include <future>
#include <sstream>

extern "C" {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#pragma GCC diagnostic pop
}

namespace cloudstorage {

namespace {

std::mutex mutex;
bool initialized;

struct ImageSize {
  int width_;
  int height_;
};

template <class T>
using Pointer = std::unique_ptr<T, std::function<void(T*)>>;

struct CallbackData {
  std::function<bool(std::chrono::system_clock::time_point)> interrupt_;
  std::chrono::system_clock::time_point start_time_;
  Pointer<AVIOContext> io_context_;
};

template <class T>
Pointer<T> make(T* d, std::function<void(T**)> f) {
  return Pointer<T>(d, [=](T* d) { f(&d); });
}

template <class T>
Pointer<T> make(T* d, std::function<void(T*)> f) {
  return Pointer<T>(d, f);
}

std::string av_error(int err) {
  char buffer[AV_ERROR_MAX_STRING_SIZE + 1] = {};
  if (av_strerror(err, buffer, AV_ERROR_MAX_STRING_SIZE) < 0)
    return "invalid error";
  else
    return buffer;
}

void check(int code, const std::string& call) {
  if (code < 0) throw std::logic_error(call + " (" + av_error(code) + ")");
}

void initialize() {
  std::unique_lock<std::mutex> lock(mutex);
  if (!initialized) {
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
    avfilter_register_all();
#endif
    av_log_set_level(AV_LOG_PANIC);
    check(avformat_network_init(), "avformat_network_init");
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    check(av_lockmgr_register([](void** data, AVLockOp op) {
            if (op == AV_LOCK_CREATE)
              *data = new std::mutex;
            else if (op == AV_LOCK_DESTROY)
              delete static_cast<std::mutex*>(*data);
            else if (op == AV_LOCK_OBTAIN)
              static_cast<std::mutex*>(*data)->lock();
            else if (op == AV_LOCK_RELEASE)
              static_cast<std::mutex*>(*data)->unlock();
            return 0;
          }),
          "av_lockmgr_register");
#endif
    initialized = true;
  }
}

Pointer<AVIOContext> create_io_context(
    ICloudProvider* provider, IItem::Pointer item, uint64_t size,
    std::chrono::system_clock::time_point start_time,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt) {
  const int BUFFER_SIZE = 1024 * 1024;
  auto buffer = static_cast<uint8_t*>(av_malloc(BUFFER_SIZE));
  struct Data {
    ICloudProvider* provider_;
    IItem::Pointer item_;
    int64_t offset_;
    uint64_t size_;
    std::chrono::system_clock::time_point start_time_;
    std::function<bool(std::chrono::system_clock::time_point)> interrupt_;
  }* data = new Data{provider, std::move(item), 0,
                     size,     start_time,      std::move(interrupt)};
  struct Download : public IDownloadFileCallback {
    void progress(uint64_t, uint64_t) override {}
    void receivedData(const char* data, uint32_t size) override {
      auto bytes = std::min<int64_t>(size, size_);
      memcpy(buffer_, data, bytes);
      size_ -= bytes;
      buffer_ += bytes;
    }
    void done(EitherError<void> e) override { semaphore_.set_value(e); }

    char* buffer_;
    int64_t size_;
    std::promise<EitherError<void>> semaphore_;
  };
  return make<AVIOContext>(
      avio_alloc_context(
          buffer, BUFFER_SIZE, 0, data,
          [](void* d, uint8_t* buffer, int size) -> int {
            auto data = reinterpret_cast<Data*>(d);
            Range range;
            range.start_ = data->offset_;
            range.size_ = std::min<int64_t>(size, data->size_ - data->offset_);
            if (range.size_ == 0) {
              return AVERROR_EOF;
            }
            auto cb = std::make_shared<Download>();
            cb->buffer_ = reinterpret_cast<char*>(buffer);
            cb->size_ = size;
            auto future = cb->semaphore_.get_future();
            auto request =
                data->provider_->downloadFileAsync(data->item_, cb, range);
            while (future.wait_for(std::chrono::milliseconds(100)) !=
                   std::future_status::ready) {
              if (data->interrupt_(data->start_time_)) {
                request->cancel();
                break;
              }
            }
            data->offset_ += size - cb->size_;
            auto error = future.get().left();
            return error ? -1 : static_cast<int>(size - cb->size_);
          },
          nullptr,
          [](void* d, int64_t offset, int whence) -> int64_t {
            auto data = reinterpret_cast<Data*>(d);
            whence &= ~AVSEEK_FORCE;
            if (whence == AVSEEK_SIZE) {
              return data->size_;
            }
            if (whence == SEEK_SET) {
              data->offset_ = offset;
            } else if (whence == SEEK_CUR) {
              data->offset_ += offset;
            } else if (whence == SEEK_END) {
              if (data->item_->size() == IItem::UnknownSize) return -1;
              data->offset_ = data->size_ + offset;
            } else {
              return -1;
            }
            return data->offset_;
          }),
      [data](AVIOContext* ctx) {
        delete data;
        av_free(ctx->buffer);
        avio_context_free(&ctx);
      });
}

Pointer<AVIOContext> create_io_context(
    std::function<uint32_t(char* data, uint32_t maxlength, uint64_t offset)>
        read_callback,
    uint64_t size, std::chrono::system_clock::time_point start_time,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt) {
  const int BUFFER_SIZE = 1024 * 1024;
  auto buffer = static_cast<uint8_t*>(av_malloc(BUFFER_SIZE));
  struct Data {
    std::function<uint32_t(char* data, uint32_t maxlength, uint64_t offset)>
        read_callback_;
    int64_t offset_;
    uint64_t size_;
    std::chrono::system_clock::time_point start_time_;
    std::function<bool(std::chrono::system_clock::time_point)> interrupt_;
  }* data = new Data{std::move(read_callback), 0, size, start_time,
                     std::move(interrupt)};
  return make<AVIOContext>(
      avio_alloc_context(
          buffer, BUFFER_SIZE, 0, data,
          [](void* d, uint8_t* buffer, int size) -> int {
            auto data = reinterpret_cast<Data*>(d);
            auto read_size = data->read_callback_(
                reinterpret_cast<char*>(buffer), size, data->offset_);
            if (read_size == 0) {
              return AVERROR_EOF;
            }
            if (data->interrupt_(data->start_time_)) {
              return -1;
            }
            data->offset_ += read_size;
            return read_size;
          },
          nullptr,
          [](void* d, int64_t offset, int whence) -> int64_t {
            auto data = reinterpret_cast<Data*>(d);
            whence &= ~AVSEEK_FORCE;
            if (whence == AVSEEK_SIZE) {
              return data->size_;
            }
            if (whence == SEEK_SET) {
              data->offset_ = offset;
            } else if (whence == SEEK_CUR) {
              data->offset_ += offset;
            } else if (whence == SEEK_END) {
              data->offset_ = data->size_ + offset;
            } else {
              return -1;
            }
            return data->offset_;
          }),
      [data](AVIOContext* ctx) {
        delete data;
        av_free(ctx->buffer);
        avio_context_free(&ctx);
      });
}

Pointer<AVFormatContext> create_format_context(
    const std::string& url,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt) {
  auto context = avformat_alloc_context();
  auto data = new CallbackData{std::move(interrupt),
                               std::chrono::system_clock::now(), nullptr};
  context->interrupt_callback.opaque = data;
  context->interrupt_callback.callback = [](void* t) -> int {
    auto d = reinterpret_cast<CallbackData*>(t);
    return d->interrupt_(d->start_time_);
  };
  int e = 0;
  if ((e = avformat_open_input(&context, url.c_str(), nullptr, nullptr)) < 0) {
    avformat_free_context(context);
    delete data;
    check(e, "avformat_open_input");
  } else if ((e = avformat_find_stream_info(context, nullptr)) < 0) {
    avformat_close_input(&context);
    delete data;
    check(e, "avformat_find_stream_info");
  }
  return make<AVFormatContext>(context, [data](AVFormatContext* d) {
    avformat_close_input(&d);
    delete data;
  });
}

Pointer<AVFormatContext> create_format_context(
    Pointer<AVIOContext> io_context,
    std::chrono::system_clock::time_point start_time,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt) {
  auto context = avformat_alloc_context();
  auto data =
      new CallbackData{std::move(interrupt), start_time, std::move(io_context)};
  context->interrupt_callback.opaque = data;
  context->interrupt_callback.callback = [](void* t) -> int {
    auto d = reinterpret_cast<CallbackData*>(t);
    return d->interrupt_(d->start_time_);
  };
  context->pb = data->io_context_.get();
  int e = 0;
  if ((e = avformat_open_input(&context, nullptr, nullptr, nullptr)) < 0) {
    avformat_free_context(context);
    delete data;
    check(e, "avformat_open_input");
  } else if ((e = avformat_find_stream_info(context, nullptr)) < 0) {
    avformat_close_input(&context);
    delete data;
    check(e, "avformat_find_stream_info");
  }
  return make<AVFormatContext>(context, [data](AVFormatContext* d) {
    avformat_close_input(&d);
    delete data;
  });
}

Pointer<AVCodecContext> create_codec_context(AVFormatContext* context,
                                             int stream_index) {
  auto codec =
      avcodec_find_decoder(context->streams[stream_index]->codecpar->codec_id);
  if (!codec) throw std::logic_error("decoder not found");
  auto codec_context =
      make<AVCodecContext>(avcodec_alloc_context3(codec), avcodec_free_context);
  check(avcodec_parameters_to_context(codec_context.get(),
                                      context->streams[stream_index]->codecpar),
        "avcodec_parameters_to_context");
  check(avcodec_open2(codec_context.get(), codec, nullptr), "avcodec_open2");
  return codec_context;
}

Pointer<AVPacket> create_packet() {
  return Pointer<AVPacket>(av_packet_alloc(),
                           [](AVPacket* packet) { av_packet_free(&packet); });
}

Pointer<AVFrame> decode_frame(AVFormatContext* context,
                              AVCodecContext* codec_context, int stream_index) {
  Pointer<AVFrame> result_frame;
  while (!result_frame) {
    auto packet = create_packet();
    auto read_packet = av_read_frame(context, packet.get());
    if (read_packet != 0 && read_packet != AVERROR_EOF) {
      check(read_packet, "av_read_frame");
    } else {
      if (read_packet == 0 && packet->stream_index != stream_index) continue;
      auto send_packet = avcodec_send_packet(
          codec_context, read_packet == AVERROR_EOF ? nullptr : packet.get());
      if (send_packet != AVERROR_EOF) check(send_packet, "avcodec_send_packet");
    }
    auto frame = make<AVFrame>(av_frame_alloc(), av_frame_free);
    auto code = avcodec_receive_frame(codec_context, frame.get());
    if (code == 0) {
      result_frame = std::move(frame);
    } else if (code == AVERROR_EOF) {
      break;
    } else if (code != AVERROR(EAGAIN)) {
      check(code, "avcodec_receive_frame");
    }
  }
  return result_frame;
}

ImageSize thumbnail_size(const ImageSize& i, int target) {
  if (i.width_ == 0 || i.height_ == 0) return {target, target};
  if (i.width_ > i.height_) {
    return {target, i.height_ * target / i.width_};
  } else {
    return {i.width_ * target / i.height_, target};
  }
}

Pointer<AVFrame> convert_frame(AVFrame* frame, ImageSize size,
                               AVPixelFormat format) {
  auto sws_context = make<SwsContext>(
      sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
                     size.width_, size.height_, format, SWS_BICUBIC, nullptr,
                     nullptr, nullptr),
      sws_freeContext);
  auto rgb_frame = make<AVFrame>(av_frame_alloc(), av_frame_free);
  av_frame_copy_props(rgb_frame.get(), frame);
  rgb_frame->format = format;
  rgb_frame->width = size.width_;
  rgb_frame->height = size.height_;
  check(av_image_alloc(rgb_frame->data, rgb_frame->linesize, size.width_,
                       size.height_, format, 32),
        "av_image_alloc");
  check(sws_scale(sws_context.get(), frame->data, frame->linesize, 0,
                  frame->height, rgb_frame->data, rgb_frame->linesize),
        "sws_scale");
  return make<AVFrame>(rgb_frame.release(), [=](AVFrame* f) {
    av_freep(&f->data);
    av_frame_free(&f);
  });
}

std::string encode_frame(AVFrame* input_frame, ThumbnailOptions options) {
  auto size =
      thumbnail_size({input_frame->width, input_frame->height}, options.size);
  auto codec = avcodec_find_encoder(
      options.codec == ThumbnailOptions::Codec::JPEG ? AV_CODEC_ID_MJPEG
                                                     : AV_CODEC_ID_PNG);
  if (!codec) throw std::logic_error("codec not found");
  int loss_ptr;
  auto frame =
      convert_frame(input_frame, size,
                    avcodec_find_best_pix_fmt_of_list(
                        codec->pix_fmts, AVPixelFormat(input_frame->format),
                        false, &loss_ptr));
  auto context =
      make<AVCodecContext>(avcodec_alloc_context3(codec), avcodec_free_context);
  context->time_base = {1, 24};
  context->pix_fmt = AVPixelFormat(frame->format);
  context->width = frame->width;
  context->height = frame->height;
  check(avcodec_open2(context.get(), codec, nullptr), "avcodec_open2");
  auto packet = create_packet();
  bool frame_sent = false, flush_sent = false;
  std::string result;
  while (true) {
    if (!frame_sent) {
      check(avcodec_send_frame(context.get(), frame.get()),
            "avcodec_send_frame");
      frame_sent = true;
    } else if (!flush_sent) {
      check(avcodec_send_frame(context.get(), nullptr), "avcodec_send_frame");
      flush_sent = true;
    }
    auto err = avcodec_receive_packet(context.get(), packet.get());
    if (err != 0) {
      if (err == AVERROR_EOF)
        break;
      else
        check(err, "avcodec_receive_packet");
    } else {
      result +=
          std::string(reinterpret_cast<char*>(packet->data), packet->size);
    }
  }
  return result;
}

Pointer<AVFilterContext> create_source_filter(AVFormatContext* format_context,
                                              int stream,
                                              AVCodecContext* codec_context,
                                              AVFilterGraph* graph) {
  auto filter =
      make<AVFilterContext>(avfilter_graph_alloc_filter(
                                graph, avfilter_get_by_name("buffer"), nullptr),
                            avfilter_free);
  if (!filter) {
    throw std::logic_error("filter buffer unavailable");
  }
  AVDictionary* d = nullptr;
  av_dict_set_int(&d, "width", codec_context->width, 0);
  av_dict_set_int(&d, "height", codec_context->height, 0);
  av_dict_set_int(&d, "pix_fmt", codec_context->pix_fmt, 0);
  av_dict_set(
      &d, "time_base",
      (std::to_string(format_context->streams[stream]->time_base.num) + "/" +
       std::to_string(format_context->streams[stream]->time_base.den))
          .c_str(),
      0);
  auto err = avfilter_init_dict(filter.get(), &d);
  av_dict_free(&d);
  check(err, "avfilter_init_dict source");
  return filter;
}

Pointer<AVFilterContext> create_sink_filter(AVFilterGraph* graph) {
  auto filter = make<AVFilterContext>(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"),
                                  nullptr),
      avfilter_free);
  if (!filter) {
    throw std::logic_error("filter buffersink unavailable");
  }
  check(avfilter_init_dict(filter.get(), nullptr), "avfilter_init_dict");
  return filter;
}

Pointer<AVFilterContext> create_thumbnail_filter(AVFilterGraph* graph) {
  auto filter = make<AVFilterContext>(
      avfilter_graph_alloc_filter(graph, avfilter_get_by_name("thumbnail"),
                                  nullptr),
      avfilter_free);
  if (!filter) {
    throw std::logic_error("filter thumbnail unavailable");
  }
  check(avfilter_init_dict(filter.get(), nullptr), "avfilter_init_dict");
  return filter;
}

Pointer<AVFilterContext> create_scale_filter(AVFilterGraph* graph,
                                             ImageSize size) {
  auto filter =
      make<AVFilterContext>(avfilter_graph_alloc_filter(
                                graph, avfilter_get_by_name("scale"), nullptr),
                            avfilter_free);
  if (!filter) {
    throw std::logic_error("filter thumbnail unavailable");
  }
  AVDictionary* d = nullptr;
  av_dict_set_int(&d, "width", size.width_, 0);
  av_dict_set_int(&d, "height", size.height_, 0);
  auto err = avfilter_init_dict(filter.get(), &d);
  av_dict_free(&d);
  check(err, "avfilter_init_dict");
  return filter;
}

Pointer<AVFrame> get_thumbnail_frame(
    Pointer<AVIOContext> io_context,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt,
    ThumbnailOptions options) {
  auto start_time = std::chrono::system_clock::now();
  auto context =
      create_format_context(std::move(io_context), start_time, interrupt);
  auto stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                    nullptr, 0);
  check(stream, "av_find_best_stream");
  if (context->duration > 0) {
    check(av_seek_frame(context.get(), -1, context->duration / 10, 0),
          "av_seek_frame");
  }
  auto codec_context = create_codec_context(context.get(), stream);
  auto size = thumbnail_size({codec_context->width, codec_context->height},
                             options.size);
  auto filter_graph =
      make<AVFilterGraph>(avfilter_graph_alloc(), avfilter_graph_free);
  auto source_filter = create_source_filter(
      context.get(), stream, codec_context.get(), filter_graph.get());
  auto sink_filter = create_sink_filter(filter_graph.get());
  auto thumbnail_filter = create_thumbnail_filter(filter_graph.get());
  auto scale_filter = create_scale_filter(filter_graph.get(), size);
  check(avfilter_link(source_filter.get(), 0, scale_filter.get(), 0),
        "avfilter_link");
  check(avfilter_link(scale_filter.get(), 0, thumbnail_filter.get(), 0),
        "avfilter_link");
  check(avfilter_link(thumbnail_filter.get(), 0, sink_filter.get(), 0),
        "avfilter_link");
  check(avfilter_graph_config(filter_graph.get(), nullptr),
        "avfilter_graph_config");
  Pointer<AVFrame> frame;
  while (auto current =
             decode_frame(context.get(), codec_context.get(), stream)) {
    frame = std::move(current);
    check(av_buffersrc_write_frame(source_filter.get(), frame.get()),
          "av_buffersrc_write_frame");
    auto received_frame = make<AVFrame>(av_frame_alloc(), av_frame_free);
    auto err = av_buffersink_get_frame(sink_filter.get(), received_frame.get());
    if (err == 0) {
      frame = std::move(received_frame);
      break;
    } else if (err != AVERROR(EAGAIN)) {
      check(err, "av_buffersink_get_frame");
    }
  }
  if (!frame) {
    throw std::logic_error("couldn't get any frame");
  }
  return frame;
}

}  // namespace

EitherError<std::string> generate_thumbnail(
    ICloudProvider* provider, IItem::Pointer item, uint64_t size,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt,
    ThumbnailOptions options) {
  try {
    initialize();
    return encode_frame(
        get_thumbnail_frame(
            create_io_context(provider, std::move(item), size,
                              std::chrono::system_clock::now(), interrupt),
            interrupt, options)
            .get(),
        options);
  } catch (const std::exception& e) {
    return Error{IHttpRequest::Failure, e.what()};
  }
}

EitherError<std::string> generate_thumbnail(
    const std::string& url, int64_t timestamp,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt,
    ThumbnailOptions options) {
  try {
    initialize();
    std::string effective_url = url;
#ifdef _WIN32
    const char* file = "file:///";
#else
    const char* file = "file://";
#endif
    const auto length = strlen(file);
    if (url.substr(0, length) == file) effective_url = url.substr(length);
    auto context = create_format_context(effective_url, std::move(interrupt));
    auto stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                      nullptr, 0);
    check(stream, "av_find_best_stream");
    check(avformat_seek_file(context.get(), -1, INT64_MIN,
                             timestamp * AV_TIME_BASE / 1000, INT64_MAX, 0),
          "avformat_seek_file");
    auto codec_context = create_codec_context(context.get(), stream);
    Pointer<AVFrame> current =
        decode_frame(context.get(), codec_context.get(), stream);
    if (!current) throw std::logic_error("couldn't get frame");
    return encode_frame(current.get(), options);
  } catch (const std::exception& e) {
    return Error{IHttpRequest::Failure, e.what()};
  }
}

EitherError<std::string> generate_thumbnail(
    ICloudProvider* provider, IItem::Pointer item, int64_t timestamp,
    uint64_t size,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt,
    ThumbnailOptions options) {
  try {
    initialize();
    auto start_time = std::chrono::system_clock::now();
    auto context =
        create_format_context(create_io_context(provider, std::move(item), size,
                                                start_time, interrupt),
                              start_time, interrupt);
    auto stream = av_find_best_stream(context.get(), AVMEDIA_TYPE_VIDEO, -1, -1,
                                      nullptr, 0);
    check(stream, "av_find_best_stream");
    check(avformat_seek_file(context.get(), -1, INT64_MIN,
                             timestamp * AV_TIME_BASE / 1000, INT64_MAX, 0),
          "avformat_seek_file");
    auto codec_context = create_codec_context(context.get(), stream);
    Pointer<AVFrame> current =
        decode_frame(context.get(), codec_context.get(), stream);
    if (!current) throw std::logic_error("couldn't get frame");
    return encode_frame(current.get(), options);
  } catch (const std::exception& e) {
    return Error{IHttpRequest::Failure, e.what()};
  }
}

EitherError<std::string> generate_thumbnail(
    std::function<uint32_t(char* data, uint32_t maxlength, uint64_t offset)>
        read_callback,
    uint64_t size,
    std::function<bool(std::chrono::system_clock::time_point)> interrupt,
    ThumbnailOptions options) {
  try {
    initialize();
    return encode_frame(
        get_thumbnail_frame(
            create_io_context(std::move(read_callback), size,
                              std::chrono::system_clock::now(), interrupt),
            interrupt, options)
            .get(),
        options);
  } catch (const std::exception& e) {
    return Error{IHttpRequest::Failure, e.what()};
  }
}

}  // namespace cloudstorage

#endif  // WITH_THUMBNAILER
