/*****************************************************************************
 * DeleteItemRequest.cpp : DeleteItemRequest implementation
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

#include "DeleteItemRequest.h"

#include "CloudProvider/CloudProvider.h"

namespace cloudstorage {

DeleteItemRequest::DeleteItemRequest(std::shared_ptr<CloudProvider> p,
                                     const IItem::Pointer& item,
                                     const DeleteItemCallback& callback)
    : Request(std::move(p), callback, [=, this](Request::Pointer request) {
        this->request(
            [=, this](util::Output stream) {
              return provider()->deleteItemRequest(*item, *stream);
            },
            [=](EitherError<Response> e) {
              if (e.left())
                request->done(e.left());
              else
                request->done(nullptr);
            });
      }) {}

DeleteItemRequest::~DeleteItemRequest() { cancel(); }

}  // namespace cloudstorage
