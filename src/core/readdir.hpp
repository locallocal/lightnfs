#pragma once

#include "backend/api.hpp"

namespace lnfs::core {

rt::Task<Result<backend::DirPage>> readdir_page(const backend::ObjPtr& dir,
                                                 const backend::Cred& cred,
                                                 uint64_t cookie,
                                                 uint32_t max_entries);

}  // namespace lnfs::core
