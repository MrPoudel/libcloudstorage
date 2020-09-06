/*****************************************************************************
 * AmazonS3.cpp : AmazonS3 implementation
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
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

#include "AmazonS3.h"

#include <json/json.h>
#include <tinyxml2.h>
#include <algorithm>
#include <iomanip>

#include "Request/RecursiveRequest.h"
#include "Utility/Utility.h"

using namespace std::placeholders;

namespace cloudstorage {

namespace {

std::string escapePath(const std::string& str) {
  std::string data = util::Url::escape(str);
  std::string slash = util::Url::escape("/");
  std::string result;
  for (size_t i = 0; i < data.size();)
    if (data.substr(i, slash.length()) == slash) {
      result += "/";
      i += slash.length();
    } else {
      result += data[i];
      i++;
    }
  return result;
}

std::string currentDate() {
  auto time =
      util::gmtime(std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count());
  std::stringstream ss;
  ss << std::put_time(&time, "%Y%m%d");
  return ss.str();
}

std::string currentDateAndTime() {
  auto time =
      util::gmtime(std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count());
  std::stringstream ss;
  ss << std::put_time(&time, "%Y%m%dT%H%M%SZ");
  return ss.str();
}

}  // namespace

AmazonS3::AmazonS3() : CloudProvider(util::make_unique<Auth>()) {}

void AmazonS3::initialize(InitData&& init_data) {
  if (init_data.token_.empty())
    init_data.token_ = credentialsToString(Json::Value(Json::objectValue));
  unpackCredentials(init_data.token_);
  {
    auto lock = auth_lock();
    setWithHint(init_data.hints_, "rewritten_endpoint",
                [&](const std::string& v) { rewritten_endpoint_ = v; });
    setWithHint(init_data.hints_, "region",
                [&](const std::string& v) { region_ = v; });
  }
  CloudProvider::initialize(std::move(init_data));
}

std::string AmazonS3::token() const {
  Json::Value json;
  json["username"] = access_id();
  json["password"] = secret();
  json["bucket"] = bucket();
  json["endpoint"] = s3_endpoint();
  return credentialsToString(json);
}

std::string AmazonS3::name() const { return "amazons3"; }

std::string AmazonS3::endpoint() const {
  auto lock = auth_lock();
  if (!rewritten_endpoint_.empty())
    return rewritten_endpoint_;
  else
    return s3_endpoint_ + "/" + bucket_;
}

IItem::Pointer AmazonS3::rootDirectory() const {
  return util::make_unique<Item>("/", "", IItem::UnknownSize,
                                 IItem::UnknownTimeStamp,
                                 IItem::FileType::Directory);
}

ICloudProvider::Hints AmazonS3::hints() const {
  auto hints = CloudProvider::hints();
  auto lock = auth_lock();
  hints.insert(
      {{"rewritten_endpoint", rewritten_endpoint_}, {"region", region_}});
  return hints;
}

AuthorizeRequest::Pointer AmazonS3::authorizeAsync() {
  auto reauth = [=](AuthorizeRequest::Pointer r,
                    AuthorizeRequest::Callback complete) {
    if (auth_callback()->userConsentRequired(*this) !=
        ICloudProvider::IAuthCallback::Status::WaitForAuthorizationCode) {
      return complete(
          Error{IHttpRequest::Unauthorized, util::Error::INVALID_CREDENTIALS});
    }
    auto code = [=](EitherError<std::string> code) {
      (void)r;
      if (code.left())
        complete(code.left());
      else {
        if (unpackCredentials(*code.right()))
          complete(nullptr);
        else
          complete(Error{IHttpRequest::Failure,
                         util::Error::INVALID_AUTHORIZATION_CODE});
      }
    };
    r->set_server(this->auth()->requestAuthorizationCode(code));
  };
  auto auth = [=](AuthorizeRequest::Pointer r,
                  AuthorizeRequest::Callback complete) {
    this->getRegion(r, [=](EitherError<void> e) {
      if (e.left()) {
        if (e.left()->code_ == IHttpRequest::Unauthorized)
          reauth(r, [=](EitherError<void> e) {
            if (e.left())
              complete(e);
            else {
              this->getRegion(r, [=](EitherError<void> e) {
                if (e.left())
                  complete(e);
                else
                  this->getEndpoint(r, complete);
              });
            }
          });
        else
          complete(e.left());
      } else
        this->getEndpoint(r, complete);
    });
  };
  return std::make_shared<AuthorizeRequest>(shared_from_this(), auth);
}

ICloudProvider::MoveItemRequest::Pointer AmazonS3::moveItemAsync(
    IItem::Pointer source, IItem::Pointer destination,
    MoveItemCallback callback) {
  using Request = RecursiveRequest<EitherError<IItem>>;
  auto visitor = [=](Request::Pointer r, IItem::Pointer item,
                     Request::CompleteCallback callback) {
    auto l = getPath("/" + source->id()).length();
    std::string new_path = destination->id() + item->id().substr(l);
    r->request(
        [=](util::Output) {
          auto request = http()->create(endpoint() + "/" + new_path, "PUT");
          if (item->type() != IItem::FileType::Directory)
            request->setHeaderParameter(
                "x-amz-copy-source", bucket() + "/" + escapePath(item->id()));
          return request;
        },
        [=](EitherError<Response> e) {
          if (e.left()) return callback(e.left());
          r->request(
              [=](util::Output) {
                return http()->create(endpoint() + "/" + escapePath(item->id()),
                                      "DELETE");
              },
              [=](EitherError<Response> e) {
                if (e.left()) return callback(e.left());
                IItem::Pointer renamed = std::make_shared<Item>(
                    getFilename(new_path), new_path, item->size(),
                    item->timestamp(), item->type());
                callback(renamed);
              });
        });
  };
  return std::make_shared<Request>(shared_from_this(), source, callback,
                                   visitor)
      ->run();
}

ICloudProvider::RenameItemRequest::Pointer AmazonS3::renameItemAsync(
    IItem::Pointer root, const std::string& name, RenameItemCallback callback) {
  using Request = RecursiveRequest<EitherError<IItem>>;
  auto new_prefix = (getPath("/" + root->id()) + "/" + name).substr(1);
  auto visitor = [=](Request::Pointer r, IItem::Pointer item,
                     Request::CompleteCallback callback) {
    auto new_path = new_prefix + "/" + item->id().substr(root->id().length());
    if (!new_path.empty() && new_path.back() == '/' &&
        item->type() != IItem::FileType::Directory)
      new_path.pop_back();
    r->request(
        [=](util::Output) {
          auto request = http()->create(endpoint() + "/" + new_path, "PUT");
          if (item->type() != IItem::FileType::Directory)
            request->setHeaderParameter(
                "x-amz-copy-source", bucket() + "/" + escapePath(item->id()));
          return request;
        },
        [=](EitherError<Response> e) {
          if (e.left()) return callback(e.left());
          r->request(
              [=](util::Output) {
                return http()->create(endpoint() + "/" + escapePath(item->id()),
                                      "DELETE");
              },
              [=](EitherError<Response> e) {
                if (e.left()) return callback(e.left());
                IItem::Pointer renamed = std::make_shared<Item>(
                    getFilename(new_path), new_path, item->size(),
                    item->timestamp(), item->type());
                callback(renamed);
              });
        });
  };
  return std::make_shared<Request>(shared_from_this(), root, callback, visitor)
      ->run();
}

IHttpRequest::Pointer AmazonS3::createDirectoryRequest(const IItem& parent,
                                                       const std::string& name,
                                                       std::ostream&) const {
  return http()->create(endpoint() + "/" + escapePath(parent.id() + name + "/"),
                        "PUT");
}

IItem::Pointer AmazonS3::createDirectoryResponse(const IItem& parent,
                                                 const std::string& name,
                                                 std::istream&) const {
  return std::make_shared<Item>(name, parent.id() + name + "/", 0,
                                IItem::UnknownTimeStamp,
                                IItem::FileType::Directory);
}

ICloudProvider::DeleteItemRequest::Pointer AmazonS3::deleteItemAsync(
    IItem::Pointer item, DeleteItemCallback callback) {
  using Request = RecursiveRequest<EitherError<void>>;
  auto visitor = [=](Request::Pointer r, IItem::Pointer item,
                     Request::CompleteCallback complete) {
    r->request(
        [=](util::Output) {
          return http()->create(endpoint() + "/" + escapePath(item->id()),
                                "DELETE");
        },
        [=](EitherError<Response> e) {
          if (e.left())
            complete(e.left());
          else
            complete(nullptr);
        });
  };
  return std::make_shared<Request>(shared_from_this(), item, callback, visitor)
      ->run();
}

ICloudProvider::GeneralDataRequest::Pointer AmazonS3::getGeneralDataAsync(
    GeneralDataCallback callback) {
  auto resolver = [=](Request<EitherError<GeneralData>>::Pointer r) {
    auto endpoint = this->s3_endpoint();
    auto bucket = this->bucket();
    GeneralData data;
    data.space_total_ = data.space_used_ = 0;
    data.username_ = endpoint == "https://s3.amazonaws.com"
                         ? bucket
                         : endpoint + "/" + bucket;
    r->done(data);
  };
  return std::make_shared<Request<EitherError<GeneralData>>>(shared_from_this(),
                                                             callback, resolver)
      ->run();
}

ICloudProvider::GetItemDataRequest::Pointer AmazonS3::getItemDataAsync(
    const std::string& id, GetItemCallback callback) {
  return std::make_shared<Request<EitherError<IItem>>>(
             shared_from_this(), callback,
             [=](Request<EitherError<IItem>>::Pointer r) {
               if (id == rootDirectory()->id()) return r->done(rootDirectory());
               if (id.empty())
                 return r->done(EitherError<IItem>(rootDirectory()));
               auto factory = [=](util::Output) {
                 auto request = http()->create(endpoint() + "/", "GET");
                 request->setParameter("list-type", "2");
                 request->setParameter("prefix", id);
                 request->setParameter("delimiter", "/");
                 return request;
               };
               r->request(factory, [=](EitherError<Response> e) {
                 if (e.left()) return r->done(e.left());
                 std::stringstream sstream;
                 sstream << e.right()->output().rdbuf();
                 tinyxml2::XMLDocument document;
                 if (document.Parse(sstream.str().c_str()) !=
                     tinyxml2::XML_SUCCESS)
                   return r->done(Error{IHttpRequest::Failure,
                                        util::Error::FAILED_TO_PARSE_XML});
                 auto node = document.RootElement();
                 auto size = IItem::UnknownSize;
                 auto timestamp = IItem::UnknownTimeStamp;
                 if (auto contents_element =
                         node->FirstChildElement("Contents")) {
                   if (auto size_element =
                           contents_element->FirstChildElement("Size"))
                     if (auto text = size_element->GetText())
                       size = std::stoull(text);
                   if (auto time_element =
                           contents_element->FirstChildElement("LastModified"))
                     if (auto text = time_element->GetText())
                       timestamp = util::parse_time(text);
                 }
                 auto type = id.back() == '/' ? IItem::FileType::Directory
                                              : IItem::FileType::Unknown;
                 auto item = std::make_shared<Item>(
                     getFilename(id), id,
                     type == IItem::FileType::Directory ? IItem::UnknownSize
                                                        : size,
                     timestamp, type);
                 if (item->type() != IItem::FileType::Directory)
                   item->set_url(getUrl(*item));
                 r->done(EitherError<IItem>(item));
               });
             })
      ->run();
}

IHttpRequest::Pointer AmazonS3::listDirectoryRequest(
    const IItem& item, const std::string& page_token, std::ostream&) const {
  auto request = http()->create(endpoint() + "/", "GET");
  request->setParameter("list-type", "2");
  request->setParameter("prefix", item.id());
  request->setParameter("delimiter", "/");
  if (!page_token.empty())
    request->setParameter("continuation-token", page_token);
  return request;
}

IHttpRequest::Pointer AmazonS3::uploadFileRequest(const IItem& directory,
                                                  const std::string& filename,
                                                  std::ostream&,
                                                  std::ostream&) const {
  return http()->create(
      endpoint() + "/" + escapePath(directory.id() + filename), "PUT");
}

IItem::Pointer AmazonS3::uploadFileResponse(const IItem& item,
                                            const std::string& filename,
                                            uint64_t size,
                                            std::istream&) const {
  return util::make_unique<Item>(filename, item.id() + filename, size,
                                 std::chrono::system_clock::now(),
                                 IItem::FileType::Unknown);
}

IHttpRequest::Pointer AmazonS3::downloadFileRequest(const IItem& item,
                                                    std::ostream&) const {
  return http()->create(endpoint() + "/" + escapePath(item.id()), "GET");
}

IItem::List AmazonS3::listDirectoryResponse(
    const IItem& parent, std::istream& stream,
    std::string& next_page_token) const {
  std::stringstream sstream;
  sstream << stream.rdbuf();
  tinyxml2::XMLDocument document;
  if (document.Parse(sstream.str().c_str()) != tinyxml2::XML_SUCCESS)
    throw std::logic_error(util::Error::FAILED_TO_PARSE_XML);
  IItem::List result;
  if (document.RootElement()->FirstChildElement("Name")) {
    for (auto child = document.RootElement()->FirstChildElement("Contents");
         child; child = child->NextSiblingElement("Contents")) {
      auto size_element = child->FirstChildElement("Size");
      if (!size_element) throw std::logic_error(util::Error::INVALID_XML);
      auto size = std::stoull(size_element->GetText());
      auto key_element = child->FirstChildElement("Key");
      if (!key_element) throw std::logic_error(util::Error::INVALID_XML);
      std::string id = key_element->GetText();
      if (size == 0 && id == parent.id()) continue;
      auto timestamp_element = child->FirstChildElement("LastModified");
      if (!timestamp_element) throw std::logic_error(util::Error::INVALID_XML);
      std::string timestamp = timestamp_element->GetText();
      auto item = util::make_unique<Item>(getFilename(id), id, size,
                                          util::parse_time(timestamp),
                                          IItem::FileType::Unknown);
      item->set_url(getUrl(*item));
      result.push_back(std::move(item));
    }
    for (auto child =
             document.RootElement()->FirstChildElement("CommonPrefixes");
         child; child = child->NextSiblingElement("CommonPrefixes")) {
      auto prefix_element = child->FirstChildElement("Prefix");
      if (!prefix_element) throw std::logic_error(util::Error::INVALID_XML);
      std::string id = prefix_element->GetText();
      auto item = util::make_unique<Item>(
          getFilename(id), id, IItem::UnknownSize, IItem::UnknownTimeStamp,
          IItem::FileType::Directory);
      result.push_back(std::move(item));
    }
    auto is_truncated_element =
        document.RootElement()->FirstChildElement("IsTruncated");
    if (!is_truncated_element) throw std::logic_error(util::Error::INVALID_XML);
    if (is_truncated_element->GetText() == std::string("true")) {
      auto next_token_element =
          document.RootElement()->FirstChildElement("NextContinuationToken");
      if (!next_token_element) throw std::logic_error(util::Error::INVALID_XML);
      next_page_token = next_token_element->GetText();
    }
  }
  return result;
}

void AmazonS3::authorizeRequest(IHttpRequest& request) const {
  if (!crypto()) throw std::runtime_error("no crypto functions provided");
  std::string region = this->region().empty() ? "us-east-1" : this->region();
  std::string current_date = currentDate();
  std::string time = currentDateAndTime();
  std::string scope = current_date + "/" + region + "/s3/aws4_request";
  util::Url url(request.url());
  request.setParameter("X-Amz-Algorithm", "AWS4-HMAC-SHA256");
  request.setParameter("X-Amz-Credential", access_id() + "/" + scope);
  request.setParameter("X-Amz-Date", time);
  request.setParameter("X-Amz-Expires", "86400");
  request.setHeaderParameter("host", url.host());

  std::vector<std::pair<std::string, std::string>> header_parameters;
  for (auto q : request.headerParameters())
    header_parameters.emplace_back(util::to_lower(q.first), q.second);
  std::sort(header_parameters.begin(), header_parameters.end());

  std::string signed_headers;
  {
    bool first = false;
    for (const auto& q : header_parameters) {
      if (first)
        signed_headers += ";";
      else
        first = true;
      signed_headers += q.first;
    }
    request.setParameter("X-Amz-SignedHeaders", signed_headers);
  }

  std::vector<std::pair<std::string, std::string>> query_parameters;
  for (auto q : request.parameters())
    query_parameters.emplace_back(q.first, q.second);
  std::sort(query_parameters.begin(), query_parameters.end());

  std::string canonical_request = request.method() + "\n" + url.path() + "\n";

  bool first = false;
  for (const auto& q : query_parameters) {
    if (first)
      canonical_request += "&";
    else
      first = true;
    canonical_request +=
        util::Url::escape(q.first) + "=" + util::Url::escape(q.second);
  }
  canonical_request += "\n";

  for (const auto& q : header_parameters)
    canonical_request += util::Url::escape(q.first) + ":" + q.second + "\n";
  canonical_request += "\n";
  canonical_request += signed_headers + "\n";
  canonical_request += "UNSIGNED-PAYLOAD";

  auto hash = std::bind(&ICrypto::sha256, crypto(), _1);
  auto sign = std::bind(&ICrypto::hmac_sha256, crypto(), _1, _2);
  auto hex = std::bind(&ICrypto::hex, crypto(), _1);

  std::string string_to_sign = "AWS4-HMAC-SHA256\n" + time + "\n" + scope +
                               "\n" + hex(hash(canonical_request));
  std::string key =
      sign(sign(sign(sign("AWS4" + secret(), current_date), region), "s3"),
           "aws4_request");
  std::string signature = hex(sign(key, string_to_sign));

  request.setParameter("X-Amz-Signature", signature);

  auto params = request.parameters();
  for (const auto& p : params)
    request.setParameter(p.first, util::Url::escape(p.second));
}

bool AmazonS3::reauthorize(int code,
                           const IHttpRequest::HeaderParameters& h) const {
  return CloudProvider::reauthorize(code, h) ||
         code == IHttpRequest::Forbidden ||
         code == IHttpRequest::PermamentRedirect || access_id().empty() ||
         secret().empty() || region().empty();
}

bool AmazonS3::isSuccess(int code,
                         const IHttpRequest::HeaderParameters& headers) const {
  return code != IHttpRequest::PermamentRedirect &&
         CloudProvider::isSuccess(code, headers);
}

std::string AmazonS3::access_id() const {
  auto lock = auth_lock();
  return access_id_;
}

std::string AmazonS3::secret() const {
  auto lock = auth_lock();
  return secret_;
}

std::string AmazonS3::bucket() const {
  auto lock = auth_lock();
  return bucket_;
}

std::string AmazonS3::s3_endpoint() const {
  auto lock = auth_lock();
  return s3_endpoint_.empty() ? "https://s3.amazonaws.com" : s3_endpoint_;
}

std::string AmazonS3::region() const {
  auto lock = auth_lock();
  return region_;
}

bool AmazonS3::unpackCredentials(const std::string& code) {
  try {
    auto lock = auth_lock();
    auto json = credentialsFromString(code);
    access_id_ = json["username"].asString();
    secret_ = json["password"].asString();
    bucket_ = json["bucket"].asString();
    s3_endpoint_ = json["endpoint"].asString();
    return true;
  } catch (const Json::Exception&) {
    return false;
  }
}

std::string AmazonS3::getUrl(const Item& item) const {
  auto request =
      http()->create(endpoint() + "/" + escapePath(item.id()), "GET");
  authorizeRequest(*request);
  std::string parameters;
  for (const auto& p : request->parameters())
    parameters += p.first + "=" + p.second + "&";
  return request->url() + "?" + parameters;
}

void AmazonS3::getRegion(const AuthorizeRequest::Pointer& r,
                         const AuthorizeRequest::AuthorizeCompleted& complete) {
  r->send(
      [=](util::Output) {
        auto r = http()->create(endpoint() + "/", "GET");
        r->setParameter("location", "");
        authorizeRequest(*r);
        return r;
      },
      [=](EitherError<Response> e) {
        if (e.left()) {
          if (e.left()->code_ == IHttpRequest::PermamentRedirect) {
            tinyxml2::XMLDocument document;
            if (document.Parse(e.left()->description_.c_str()) != 0)
              return complete(Error{IHttpRequest::Failure,
                                    util::Error::FAILED_TO_PARSE_XML});
            auto endpoint =
                document.RootElement()->FirstChildElement("Endpoint");
            if (!endpoint || !endpoint->GetText())
              return complete(
                  Error{IHttpRequest::Failure, util::Error::INVALID_XML});
            {
              auto lock = auth_lock();
              rewritten_endpoint_ = endpoint->GetText();
            }
            return getRegion(r, complete);
          }
          complete(e.left());
        } else {
          tinyxml2::XMLDocument document;
          if (document.Parse(e.right()->output().str().c_str()) != 0)
            return complete(
                Error{IHttpRequest::Failure, util::Error::FAILED_TO_PARSE_XML});
          auto location = document.RootElement();
          if (!location || !location->GetText())
            return complete(
                Error{IHttpRequest::Failure, util::Error::INVALID_XML});
          {
            auto lock = auth_lock();
            region_ = location->GetText();
          }
          complete(nullptr);
        }
      });
}

void AmazonS3::getEndpoint(
    const AuthorizeRequest::Pointer& r,
    const AuthorizeRequest::AuthorizeCompleted& complete) {
  r->send(
      [=](util::Output) {
        auto r = http()->create(endpoint() + "/", "GET");
        authorizeRequest(*r);
        return r;
      },
      [=](EitherError<Response> e) {
        if (e.left()) {
          if (e.left()->code_ == IHttpRequest::PermamentRedirect) {
            tinyxml2::XMLDocument document;
            if (document.Parse(e.left()->description_.c_str()) != 0)
              return complete(Error{IHttpRequest::Failure,
                                    util::Error::FAILED_TO_PARSE_XML});
            auto endpoint =
                document.RootElement()->FirstChildElement("Endpoint");
            if (!endpoint || !endpoint->GetText())
              return complete(
                  Error{IHttpRequest::Failure, util::Error::INVALID_XML});
            {
              auto lock = auth_lock();
              rewritten_endpoint_ = endpoint->GetText();
            }
            return complete(nullptr);
          } else {
            complete(e.left());
          }
        } else {
          complete(nullptr);
        }
      });
}

}  // namespace cloudstorage
