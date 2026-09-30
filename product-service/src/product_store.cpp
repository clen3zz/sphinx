// SPDX-License-Identifier: Apache-2.0
#include <sphinx/product_store.h>

#include <stdexcept>

namespace sphinx {

std::vector<std::optional<Product>> ProductStore::find_many(const std::vector<std::uint64_t>& ids) {
  if (ids.size() > max_product_batch_size) {
    throw std::invalid_argument{"product store batch exceeds 32 IDs"};
  }
  for (const auto id : ids) {
    if (id == 0) {
      throw std::invalid_argument{"product store batch IDs must be positive"};
    }
  }

  std::vector<std::optional<Product>> products;
  products.reserve(ids.size());
  for (const auto id : ids) {
    products.push_back(find(id));
  }
  return products;
}

}  // namespace sphinx
