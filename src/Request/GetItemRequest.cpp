/*****************************************************************************
 * GetItemRequest.cpp : GetItemRequest implementation
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

#include "GetItemRequest.h"

#include "CloudProvider/CloudProvider.h"

namespace cloudstorage {

GetItemRequest::GetItemRequest(std::shared_ptr<CloudProvider> p,
                               const std::string& path,
                               const Callback& callback)
    : Request(std::move(p), callback, [=, this](Request::Pointer) {
        if (path.empty() || path.front() != '/')
          return done(
              Error{IHttpRequest::Forbidden, util::Error::INVALID_PATH});
        work(provider()->rootDirectory(), path, callback);
      }) {}

GetItemRequest::~GetItemRequest() { cancel(); }

IItem::Pointer GetItemRequest::getItem(const IItem::List& items,
                                       const std::string& name) const {
  for (const IItem::Pointer& i : items)
    if (i->filename() == name) return i;
  return nullptr;
}

void GetItemRequest::work(const IItem::Pointer& item, const std::string& p,
                          const Callback& complete) {
  if (!item)
    return done(Error{IHttpRequest::NotFound, util::Error::ITEM_NOT_FOUND});
  if (p.empty() || p.size() == 1) return done(item);

  auto path = p.substr(1);
  auto it = path.find_first_of('/');
  std::string name = it == std::string::npos
                         ? path
                         : std::string(path.begin(), path.begin() + it),
              rest = it == std::string::npos
                         ? ""
                         : std::string(path.begin() + it, path.end());
  auto request = this->shared_from_this();
  make_subrequest(&CloudProvider::listDirectorySimpleAsync, item,
                  [=, this](EitherError<IItem::List> e) {
                    if (e.left())
                      request->done(e.left());
                    else
                      work(getItem(*e.right(), name), rest, complete);
                  });
}

}  // namespace cloudstorage
