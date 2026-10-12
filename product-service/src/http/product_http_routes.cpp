// SPDX-License-Identifier: Apache-2.0
#include "product_http_routes.h"

#include <sphinx/product/application/product_metrics.h>
#include <sphinx/product/bootstrap/product_config.h>
#include <sphinx/product/domain/product.h>

#include <array>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sphinx {
namespace {

using Json = nlohmann::json;

void set_json_response(httplib::Response& response, int status, std::string body) {
  response.status = status;
  response.set_header("Content-Type", "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.body = std::move(body);
}

void set_error_response(httplib::Response& response, int status, const char* error) {
  set_json_response(response, status, std::string{"{\"error\":\""} + error + "\"}");
}

template <typename Handler>
auto product_handler(Handler handler) {
  return
      [handler = std::move(handler)](const httplib::Request& request, httplib::Response& response) {
        try {
          handler(request, response);
        } catch (...) {
          if (request.method == "GET") {
            response.set_header("X-Cache", "NOT_CHECKED");
          }
          set_error_response(response, 500, "internal_error");
        }
      };
}

Json product_json(const Product& product) {
  return Json{{"id", product.id},
              {"name", product.name},
              {"price_cents", product.price_cents},
              {"version", product.version}};
}

const char* cache_source_name(CacheSource source) noexcept {
  switch (source) {
    case CacheSource::Hit:
      return "HIT";
    case CacheSource::Miss:
      return "MISS";
    case CacheSource::Bypass:
      return "BYPASS";
    case CacheSource::Corrupt:
      return "CORRUPT";
    case CacheSource::NotChecked:
      return "NOT_CHECKED";
  }
  return "NOT_CHECKED";
}

struct ProductHttpError {
  int status;
  const char* name;
};

ProductHttpError product_http_error(ProductStatus status) noexcept {
  switch (status) {
    case ProductStatus::InvalidArgument:
      return {400, "invalid_argument"};
    case ProductStatus::NotFound:
      return {404, "not_found"};
    case ProductStatus::Conflict:
      return {409, "conflict"};
    case ProductStatus::StoreUnavailable:
      return {503, "store_unavailable"};
    case ProductStatus::ReadBusy:
      return {503, "read_busy"};
    case ProductStatus::CommitUnknown:
      return {503, "commit_unknown"};
    case ProductStatus::Ok:
    case ProductStatus::InternalError:
      return {500, "internal_error"};
  }
  return {500, "internal_error"};
}

void set_product_status_error(httplib::Response& response, ProductStatus status) {
  const auto error = product_http_error(status);
  set_error_response(response, error.status, error.name);
  if (status == ProductStatus::ReadBusy) {
    response.set_header("Retry-After", "1");
  }
}

std::optional<std::uint64_t> parse_positive_id(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::uint64_t id = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), id, 10);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || id == 0) {
    return std::nullopt;
  }
  return id;
}

std::optional<std::uint64_t> parse_product_id(const httplib::Request& request) {
  return parse_positive_id(request.matches[1].str());
}

/// 只接受无查询参数或原样的 "fresh=1"；其他查询形式返回 nullopt。
std::optional<bool> parse_fresh_query(const httplib::Request& request) {
  const std::string_view target{request.target};
  const auto query_start = target.find('?');
  if (query_start == std::string_view::npos) {
    return false;
  }
  if (target.substr(query_start + 1) != "fresh=1") {
    return std::nullopt;
  }
  return true;
}

struct ProductBatchQuery {
  std::vector<std::uint64_t> ids;
  bool bypass_cache = false;
};

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  do {
    const auto end = text.find(separator, start);
    parts.push_back(text.substr(start, end == text.npos ? end : end - start));
    if (end == text.npos) {
      break;
    }
    start = end + 1;
  } while (true);
  return parts;
}

std::optional<ProductBatchQuery> parse_product_batch_query(const httplib::Request& request) {
  const std::string_view target{request.target};
  const auto query_start = target.find('?');
  if (query_start == target.npos) {
    return std::nullopt;
  }
  ProductBatchQuery query;
  for (const auto parameter : split(target.substr(query_start + 1), '&')) {
    const auto equals = parameter.find('=');
    if (equals == parameter.npos) {
      return std::nullopt;
    }
    const auto name = parameter.substr(0, equals);
    const auto value = parameter.substr(equals + 1);
    if (name == "fresh") {
      if (query.bypass_cache || value != "1") {
        return std::nullopt;
      }
      query.bypass_cache = true;
    } else if (name == "ids" && query.ids.empty()) {
      for (const auto text : split(value, ',')) {
        const auto id = parse_positive_id(text);
        if (!id || query.ids.size() >= max_product_batch_size) {
          return std::nullopt;
        }
        query.ids.push_back(*id);
      }
    } else {
      return std::nullopt;
    }
  }
  return query.ids.empty() ? std::nullopt : std::optional{std::move(query)};
}

bool is_json_content_type(const httplib::Request& request) {
  if (request.get_header_value_count("Content-Type") != 1) {
    return false;
  }
  const auto value = request.get_header_value("Content-Type");
  return value == "application/json" || value == "application/json; charset=utf-8";
}

bool parse_unsigned_integer(const Json& value, std::uint64_t* result) {
  if (value.is_number_unsigned()) {
    *result = value.get<std::uint64_t>();
    return true;
  }
  if (!value.is_number_integer()) {
    return false;
  }
  const auto signed_value = value.get<std::int64_t>();
  if (signed_value < 0) {
    return false;
  }
  *result = static_cast<std::uint64_t>(signed_value);
  return true;
}

std::optional<UpdateProductRequest> parse_update_request(const httplib::Request& request,
                                                         std::uint64_t id) {
  const Json body = Json::parse(request.body.begin(), request.body.end(), nullptr, false);
  if (body.is_discarded() || !body.is_object() || body.size() != 3 || !body.contains("name") ||
      !body.contains("price_cents") || !body.contains("expected_version")) {
    return std::nullopt;
  }

  const auto& name_value = body.at("name");
  if (!name_value.is_string()) {
    return std::nullopt;
  }
  auto name = name_value.get<std::string>();
  std::uint64_t price_cents = 0;
  std::uint64_t expected_version = 0;
  if (!parse_unsigned_integer(body.at("price_cents"), &price_cents) ||
      !parse_unsigned_integer(body.at("expected_version"), &expected_version)) {
    return std::nullopt;
  }
  return UpdateProductRequest{id, std::move(name), price_cents, expected_version};
}

void handle_product_result(httplib::Response& response, ProductStatus status,
                           const std::optional<Product>& product) {
  if (status == ProductStatus::Ok) {
    assert(product);
    set_json_response(response, 200, product_json(*product).dump());
  } else {
    set_product_status_error(response, status);
  }
}

void handle_get_products_result(httplib::Response& response, const GetProductsResult& result) {
  if (result.status != ProductStatus::Ok) {
    response.set_header("X-Cache", "NOT_CHECKED");
    set_product_status_error(response, result.status);
    return;
  }

  Json body;
  body["items"] = Json::array();
  std::optional<CacheSource> common_source;
  bool mixed_sources = false;
  for (const auto& item : result.items) {
    if (!common_source) {
      common_source = item.result.cache_source;
    } else if (*common_source != item.result.cache_source) {
      mixed_sources = true;
    }

    Json value;
    value["id"] = item.id;
    if (item.result.status == ProductStatus::Ok) {
      assert(item.result.product);
      value["product"] = product_json(*item.result.product);
    } else {
      value["error"] = product_http_error(item.result.status).name;
    }
    body["items"].push_back(std::move(value));
  }

  response.set_header("X-Cache", common_source
                                     ? (mixed_sources ? "MIXED" : cache_source_name(*common_source))
                                     : "NOT_CHECKED");
  set_json_response(response, 200, body.dump());
}

bool is_product_path(std::string_view path) noexcept {
  return path == "/products" || path.substr(0, sizeof("/products/") - 1) == "/products/";
}

const char* cache_breaker_state_name(CacheBreakerState state) noexcept {
  switch (state) {
    case CacheBreakerState::Closed:
      return "closed";
    case CacheBreakerState::Open:
      return "open";
    case CacheBreakerState::HalfOpen:
      return "half_open";
  }
  return "closed";
}

Json make_metrics_response(const ProductSharedState& shared, CacheBackend backend,
                           CachePolicyMode policy) {
  constexpr std::array<std::string_view, product_metric_count> metric_names{
      "get_requests",           "batch_requests",        "update_requests",
      "request_unique_ids",     "cache_lookup_keys",     "cache_hits",
      "negative_hits",          "cache_misses",          "cache_corrupt",
      "cache_read_failures",    "cache_fill_failures",   "cache_invalidation_failures",
      "cache_cleanup_failures", "store_read_operations", "store_read_ids",
      "store_read_failures",    "read_leaders",          "read_followers",
      "read_rejected",          "read_wait_timeouts",    "read_admission_rejected",
      "cache_circuit_bypasses",
  };
  const auto metrics = shared.metrics.snapshot();
  Json counters = Json::object();
  for (std::size_t index = 0; index < metric_names.size(); ++index) {
    counters[std::string{metric_names[index]}] = metrics.counters[index];
  }

  const auto reads_active_keys = shared.reads.active_key_count();
  const auto reads_active_loads = shared.reads.active_load_count();
  const auto breaker = shared.breaker.snapshot();
  return Json{{"backend", backend == CacheBackend::Redis ? "redis" : "sphinx"},
              {"policy", policy == CachePolicyMode::Protected ? "protected" : "basic"},
              {"counters", std::move(counters)},
              {"read_coordinator",
               Json{{"active_keys", reads_active_keys}, {"active_loads", reads_active_loads}}},
              {"cache_breaker", Json{{"state", cache_breaker_state_name(breaker.state)},
                                     {"consecutive_failures", breaker.consecutive_failures},
                                     {"retry_after_milliseconds", breaker.retry_after.count()}}}};
}

}  // namespace

void install_product_routes(httplib::Server& server, const ProductSharedState& shared,
                            const ProductRuntimeConfig& config) {
  server.set_post_routing_handler([](const httplib::Request&, httplib::Response& response) {
    response.set_header("Cache-Control", "no-store");
  });
  server.set_error_handler([](const httplib::Request& request, httplib::Response& response) {
    if (response.status == 413) {
      set_error_response(response, 413, "payload_too_large");
      return httplib::Server::HandlerResponse::Handled;
    }
    if (response.status == 404 && request.method == "GET" && is_product_path(request.path)) {
      response.set_header("X-Cache", "NOT_CHECKED");
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  server.Get("/metrics", [&shared, &config](const httplib::Request&, httplib::Response& response) {
    set_json_response(
        response, 200,
        make_metrics_response(shared, config.cache.backend, config.cache_policy.mode).dump());
  });

  server.Get("/products",
             product_handler([](const httplib::Request& request, httplib::Response& response) {
               const auto query = parse_product_batch_query(request);
               if (!query) {
                 response.set_header("X-Cache", "NOT_CHECKED");
                 set_error_response(response, 400, "invalid_argument");
                 return;
               }
               handle_get_products_result(
                   response, current_product_service().get_many(query->ids, query->bypass_cache));
             }));

  server.Get(R"(/products/([^/]+))",
             product_handler([](const httplib::Request& request, httplib::Response& response) {
               // HTTP 层只解析请求并映射响应；缓存命中、回源和结果分类由 ProductService 决定。
               const auto id = parse_product_id(request);
               const auto fresh = parse_fresh_query(request);
               if (!id || !fresh) {
                 response.set_header("X-Cache", "NOT_CHECKED");
                 set_error_response(response, 400, "invalid_argument");
                 return;
               }
               const auto result = current_product_service().get(*id, *fresh);
               response.set_header("X-Cache", cache_source_name(result.cache_source));
               handle_product_result(response, result.status, result.product);
             }));

  server.Put(R"(/products/([^/]+))",
             product_handler([](const httplib::Request& request, httplib::Response& response) {
               // 更新请求携带 expected_version，交给业务层和 MySQL 事务完成并发检查。
               const auto id = parse_product_id(request);
               if (!id) {
                 set_error_response(response, 400, "invalid_argument");
                 return;
               }
               if (!is_json_content_type(request)) {
                 set_error_response(response, 415, "unsupported_media_type");
                 return;
               }
               const auto update = parse_update_request(request, *id);
               if (!update) {
                 set_error_response(response, 400, "invalid_argument");
                 return;
               }
               const auto result = current_product_service().update(*update);
               if (result.cache_invalidation_failed) {
                 response.set_header("X-Cache-Invalidation", "failed");
                 std::cerr << "product cache invalidation failed id=" << *id << '\n';
               }
               handle_product_result(response, result.status, result.product);
             }));
}

}  // namespace sphinx
