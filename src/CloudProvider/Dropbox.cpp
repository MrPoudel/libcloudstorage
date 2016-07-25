/*****************************************************************************
 * Dropbox.cpp : implementation of Dropbox
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

#include "Dropbox.h"

#include <json/json.h>
#include <sstream>

#include "Utility/HttpRequest.h"
#include "Utility/Item.h"
#include "Utility/Utility.h"

#include "Request/Request.h"

namespace cloudstorage {

Dropbox::Dropbox() : CloudProvider(make_unique<Auth>()) {}

std::string Dropbox::name() const { return "dropbox"; }

IItem::Pointer Dropbox::rootDirectory() const {
  return make_unique<Item>("/", "", IItem::FileType::Directory);
}

bool Dropbox::reauthorize(int code) const { return code == 400 || code == 401; }

ICloudProvider::GetItemDataRequest::Pointer Dropbox::getItemDataAsync(
    const std::string& id, GetItemDataCallback callback) {
  auto f = make_unique<Request<IItem::Pointer>>(shared_from_this());
  f->set_resolver(
      [this, id, callback](Request<IItem::Pointer>* r) -> IItem::Pointer {
        auto item_data = [r, id](std::ostream& input) {
          auto request = make_unique<HttpRequest>(
              "https://api.dropboxapi.com/2/files/get_metadata",
              HttpRequest::Type::POST);
          request->setHeaderParameter("Content-Type", "application/json");
          Json::Value parameter;
          parameter["path"] = id;
          parameter["include_media_info"] = true;
          input << Json::FastWriter().write(parameter);
          return request;
        };
        std::stringstream output;
        if (!HttpRequest::isSuccess(r->sendRequest(item_data, output))) {
          callback(nullptr);
          return nullptr;
        }
        Json::Value response;
        output >> response;
        auto item = toItem(response);
        if (item->type() == IItem::FileType::Directory) {
          callback(item);
          return item;
        }
        auto temporary_link = [r, id](std::ostream& input) {
          auto request = make_unique<HttpRequest>(
              "https://api.dropboxapi.com/2/files/get_temporary_link",
              HttpRequest::Type::POST);
          request->setHeaderParameter("Content-Type", "application/json");

          Json::Value parameter;
          parameter["path"] = id;
          input << Json::FastWriter().write(parameter);
          return request;
        };
        if (!HttpRequest::isSuccess(r->sendRequest(temporary_link, output))) {
          callback(item);
          return item;
        }
        output >> response;
        static_cast<Item*>(item.get())->set_url(response["link"].asString());
        callback(item);
        return item;
      });
  return f;
}

HttpRequest::Pointer Dropbox::listDirectoryRequest(
    const IItem& item, const std::string& page_token,
    std::ostream& input_stream) const {
  if (!page_token.empty()) {
    HttpRequest::Pointer request = make_unique<HttpRequest>(
        "https://api.dropboxapi.com/2/files/list_folder/continue",
        HttpRequest::Type::POST);
    Json::Value input;
    input["cursor"] = page_token;
    input_stream << input;
    return request;
  }
  HttpRequest::Pointer request =
      make_unique<HttpRequest>("https://api.dropboxapi.com/2/files/list_folder",
                               HttpRequest::Type::POST);
  request->setHeaderParameter("Content-Type", "application/json");

  Json::Value parameter;
  parameter["path"] = item.id();
  parameter["include_media_info"] = true;
  input_stream << Json::FastWriter().write(parameter);
  return request;
}

HttpRequest::Pointer Dropbox::uploadFileRequest(const IItem& directory,
                                                const std::string& filename,
                                                std::ostream&,
                                                std::ostream&) const {
  const Item& item = static_cast<const Item&>(directory);
  HttpRequest::Pointer request = make_unique<HttpRequest>(
      "https://content.dropboxapi.com/1/files_put/auto/" + item.id() + filename,
      HttpRequest::Type::PUT);
  return request;
}

HttpRequest::Pointer Dropbox::downloadFileRequest(const IItem& f,
                                                  std::ostream&) const {
  const Item& item = static_cast<const Item&>(f);
  HttpRequest::Pointer request = make_unique<HttpRequest>(
      "https://content.dropboxapi.com/2/files/download",
      HttpRequest::Type::POST);
  request->setHeaderParameter("Content-Type", "");
  Json::Value parameter;
  parameter["path"] = item.id();
  std::string str = Json::FastWriter().write(parameter);
  str.pop_back();
  request->setHeaderParameter("Dropbox-API-arg", str);
  return request;
}

HttpRequest::Pointer Dropbox::getThumbnailRequest(const IItem& f,
                                                  std::ostream&) const {
  const Item& item = static_cast<const Item&>(f);
  HttpRequest::Pointer request = make_unique<HttpRequest>(
      "https://content.dropboxapi.com/2/files/get_thumbnail",
      HttpRequest::Type::POST);
  request->setHeaderParameter("Content-Type", "");

  Json::Value parameter;
  parameter["path"] = item.id();
  std::string str = Json::FastWriter().write(parameter);
  str.pop_back();
  request->setHeaderParameter("Dropbox-API-arg", str);
  return request;
}

HttpRequest::Pointer Dropbox::deleteItemRequest(
    const IItem& item, std::ostream& input_stream) const {
  auto request = make_unique<HttpRequest>(
      "https://api.dropboxapi.com/2/files/delete", HttpRequest::Type::POST);
  request->setHeaderParameter("Content-Type", "application/json");
  Json::Value parameter;
  parameter["path"] = item.id();
  input_stream << parameter;
  return request;
}

HttpRequest::Pointer Dropbox::createDirectoryRequest(
    const IItem& item, const std::string& name, std::ostream& input) const {
  auto request = make_unique<HttpRequest>(
      "https://api.dropboxapi.com/2/files/create_folder",
      HttpRequest::Type::POST);
  request->setHeaderParameter("Content-Type", "application/json");
  Json::Value parameter;
  parameter["path"] = item.id() + "/" + name;
  input << parameter;
  return request;
}

HttpRequest::Pointer Dropbox::moveItemRequest(const IItem& source,
                                              const IItem& destination,
                                              std::ostream& stream) const {
  auto request = make_unique<HttpRequest>(
      "https://api.dropboxapi.com/2/files/move", HttpRequest::Type::POST);
  request->setHeaderParameter("Content-Type", "application/json");
  Json::Value json;
  json["from_path"] = source.id();
  json["to_path"] = destination.id() + "/" + source.filename();
  stream << json;
  return request;
}

std::vector<IItem::Pointer> Dropbox::listDirectoryResponse(
    std::istream& stream, std::string& next_page_token) const {
  Json::Value response;
  stream >> response;

  std::vector<IItem::Pointer> result;
  for (const Json::Value& v : response["entries"]) result.push_back(toItem(v));
  if (response["has_more"].asBool()) {
    next_page_token = response["cursor"].asString();
  }
  return result;
}

IItem::Pointer Dropbox::createDirectoryResponse(std::istream& stream) const {
  Json::Value response;
  stream >> response;
  return toItem(response);
}

IItem::Pointer Dropbox::toItem(const Json::Value& v) {
  IItem::FileType type = IItem::FileType::Unknown;
  if (v[".tag"].asString() == "folder")
    type = IItem::FileType::Directory;
  else {
    std::string file_type = v["media_info"]["metadata"][".tag"].asString();
    if (file_type == "video")
      type = IItem::FileType::Video;
    else if (file_type == "photo")
      type = IItem::FileType::Image;
  }
  return make_unique<Item>(v["name"].asString(), v["path_display"].asString(),
                           type);
}

Dropbox::Auth::Auth() {
  set_client_id("ktryxp68ae5cicj");
  set_client_secret("6evu94gcxnmyr59");
}

std::string Dropbox::Auth::authorizeLibraryUrl() const {
  std::string url = "https://www.dropbox.com/oauth2/authorize?";
  url += "response_type=code&";
  url += "client_id=" + client_id() + "&";
  url += "redirect_uri=" + redirect_uri() + "&";
  return url;
}

IAuth::Token::Pointer Dropbox::Auth::fromTokenString(
    const std::string& str) const {
  Token::Pointer token = make_unique<Token>();
  token->token_ = str;
  token->refresh_token_ = str;
  token->expires_in_ = -1;
  return token;
}

HttpRequest::Pointer Dropbox::Auth::exchangeAuthorizationCodeRequest(
    std::ostream&) const {
  HttpRequest::Pointer request = make_unique<HttpRequest>(
      "https://api.dropboxapi.com/oauth2/token", HttpRequest::Type::POST);
  request->setParameter("grant_type", "authorization_code");
  request->setParameter("client_id", client_id());
  request->setParameter("client_secret", client_secret());
  request->setParameter("redirect_uri", redirect_uri());
  request->setParameter("code", authorization_code());
  return request;
}

HttpRequest::Pointer Dropbox::Auth::refreshTokenRequest(std::ostream&) const {
  return nullptr;
}

IAuth::Token::Pointer Dropbox::Auth::exchangeAuthorizationCodeResponse(
    std::istream& stream) const {
  Json::Value response;
  stream >> response;

  Token::Pointer token = make_unique<Token>();
  token->token_ = response["access_token"].asString();
  token->refresh_token_ = token->token_;
  token->expires_in_ = -1;
  return token;
}

IAuth::Token::Pointer Dropbox::Auth::refreshTokenResponse(std::istream&) const {
  return nullptr;
}

}  // namespace cloudstorage
