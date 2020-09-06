/*****************************************************************************
 * AmazonS3.h : AmazonS3 headers
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

#ifndef AMAZONS3_H
#define AMAZONS3_H

#include "CloudProvider.h"

#include "Utility/Item.h"

namespace cloudstorage {

/**
 * AmazonS3 requires computing HMAC-SHA256 hashes, so it requires a valid
 * ICrypto implementation. Be careful about renaming and moving directories,
 * because there has to be an http request per each of its subelement. Buckets
 * are listed as root directory's children, renaming and moving them doesn't
 * work. Token in this case is a base64 encoded json with fields
 * username (access_id), password (secret_key), region.
 */
class AmazonS3 : public CloudProvider {
 public:
  AmazonS3();

  void initialize(InitData&& data) override;

  std::string token() const override;
  std::string name() const override;
  std::string endpoint() const override;
  IItem::Pointer rootDirectory() const override;
  Hints hints() const override;

  AuthorizeRequest::Pointer authorizeAsync() override;
  GetItemDataRequest::Pointer getItemDataAsync(const std::string& id,
                                               GetItemDataCallback f) override;
  MoveItemRequest::Pointer moveItemAsync(IItem::Pointer source,
                                         IItem::Pointer destination,
                                         MoveItemCallback) override;
  RenameItemRequest::Pointer renameItemAsync(IItem::Pointer item,
                                             const std::string&,
                                             RenameItemCallback) override;
  DeleteItemRequest::Pointer deleteItemAsync(IItem::Pointer,
                                             DeleteItemCallback) override;
  GeneralDataRequest::Pointer getGeneralDataAsync(GeneralDataCallback) override;

  IHttpRequest::Pointer createDirectoryRequest(const IItem&,
                                               const std::string& name,
                                               std::ostream&) const override;
  IHttpRequest::Pointer listDirectoryRequest(
      const IItem&, const std::string& page_token,
      std::ostream& input_stream) const override;
  IHttpRequest::Pointer uploadFileRequest(
      const IItem& directory, const std::string& filename,
      std::ostream& prefix_stream, std::ostream& suffix_stream) const override;
  IHttpRequest::Pointer downloadFileRequest(
      const IItem&, std::ostream& input_stream) const override;

  IItem::List listDirectoryResponse(
      const IItem&, std::istream&, std::string& next_page_token) const override;
  IItem::Pointer createDirectoryResponse(const IItem& parent,
                                         const std::string& name,
                                         std::istream& response) const override;
  IItem::Pointer uploadFileResponse(const IItem& parent,
                                    const std::string& filename, uint64_t,
                                    std::istream& response) const override;

  void authorizeRequest(IHttpRequest&) const override;
  bool reauthorize(int, const IHttpRequest::HeaderParameters&) const override;
  bool isSuccess(int code,
                 const IHttpRequest::HeaderParameters&) const override;

  std::string access_id() const;
  std::string secret() const;
  std::string region() const;
  std::string bucket() const;
  std::string s3_endpoint() const;

 private:
  bool unpackCredentials(const std::string&) override;
  std::string getUrl(const Item&) const;
  void getRegion(const AuthorizeRequest::Pointer& r,
                 const AuthorizeRequest::AuthorizeCompleted& complete);
  void getEndpoint(const AuthorizeRequest::Pointer& r,
                   const AuthorizeRequest::AuthorizeCompleted& complete);

  std::string access_id_;
  std::string secret_;
  std::string region_;
  std::string bucket_;
  std::string s3_endpoint_;
  std::string rewritten_endpoint_;
};

}  // namespace cloudstorage

#endif  // AMAZONS3_H
