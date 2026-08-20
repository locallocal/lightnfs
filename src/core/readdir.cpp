#include "core/readdir.hpp"

#include <cerrno>
#include <limits>

namespace lnfs::core {

rt::Task<Result<backend::DirPage>> readdir_page(const backend::ObjPtr& dir,
                                                 const backend::Cred& cred,
                                                 uint64_t cookie,
                                                 uint32_t max_entries) {
  if (!dir || dir->type() != backend::FType::kDir) co_return Err(errno_from(ENOTDIR));
  backend::DirPage out;
  if (max_entries == 0) co_return out;
  auto dir_attr = co_await dir->getattr();
  if (!dir_attr) co_return Err(dir_attr.error());
  if (cookie == 0) {
    out.ents.push_back({.name = ".", .cookie = 1, .fileid = dir_attr->fileid,
                        .attr = *dir_attr, .oid = dir->id()});
    if (out.ents.size() == max_entries) co_return out;
  }
  if (cookie <= 1) {
    auto parent = co_await dir->lookup(cred, "..");
    if (!parent) co_return Err(parent.error());
    auto parent_attr = co_await (*parent)->getattr();
    if (!parent_attr) co_return Err(parent_attr.error());
    out.ents.push_back({.name = "..", .cookie = 2, .fileid = parent_attr->fileid,
                        .attr = *parent_attr, .oid = (*parent)->id()});
    if (out.ents.size() == max_entries) co_return out;
  }
  uint64_t backend_cookie = cookie <= 2 ? 0 : cookie - 3;
  auto page = co_await dir->readdir(cred, backend_cookie,
                                    max_entries - static_cast<uint32_t>(out.ents.size()));
  if (!page) co_return Err(page.error());
  for (auto& ent : page->ents) {
    if (ent.cookie > std::numeric_limits<uint64_t>::max() - 3)
      co_return Err(errno_from(EOVERFLOW));
    ent.cookie += 3;
    out.ents.push_back(std::move(ent));
  }
  out.eof = page->eof;
  co_return out;
}

}  // namespace lnfs::core
