// SPDX-License-Identifier: Apache-2.0
#include "product_http_routes.h"

#include <sphinx/product/application/product_metrics.h>
#include <sphinx/product/domain/product.h>

#include <array>
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

void set_internal_error(httplib::Response& response) {
  set_error_response(response, 500, "internal_error");
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
  return request.matches.size() < 2 ? std::nullopt : parse_positive_id(request.matches[1].str());
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

std::optional<ProductBatchQuery> parse_product_batch_query(const httplib::Request& request) {
  const std::string_view target{request.target};
  const auto query_start = target.find('?');
  if (query_start == std::string_view::npos ||
      target.find('?', query_start + 1) != std::string_view::npos) {
    return std::nullopt;
  }

  ProductBatchQuery query;
  const std::string_view raw_query = target.substr(query_start + 1);
  std::size_t token_start = 0;
  while (token_start <= raw_query.size()) {
    const auto separator = raw_query.find('&', token_start);
    const auto token_end = separator == std::string_view::npos ? raw_query.size() : separator;
    const auto parameter = raw_query.substr(token_start, token_end - token_start);
    if (parameter.empty()) {
      return std::nullopt;
    }
    const auto equals = parameter.find('=');
    if (equals == std::string_view::npos) {
      return std::nullopt;
    }
    const auto name = parameter.substr(0, equals);
    const auto value = parameter.substr(equals + 1);

    if (name == "ids") {
      if (!query.ids.empty() || value.empty()) {
        return std::nullopt;
      }
      std::size_t id_start = 0;
      while (id_start <= value.size()) {
        const auto comma = value.find(',', id_start);
        const auto id_end = comma == std::string_view::npos ? value.size() : comma;
        const auto text = value.substr(id_start, id_end - id_start);
        if (text.empty() || query.ids.size() >= max_product_batch_size) {
          return std::nullopt;
        }
        const auto id = parse_positive_id(text);
        if (!id) {
          return std::nullopt;
        }
        query.ids.push_back(*id);
        if (comma == std::string_view::npos) {
          break;
        }
        id_start = comma + 1;
      }
    } else if (name == "fresh") {
      if (query.bypass_cache || value != "1") {
        return std::nullopt;
      }
      query.bypass_cache = true;
    } else {
      return std::nullopt;
    }

    if (separator == std::string_view::npos) {
      break;
    }
    token_start = separator + 1;
  }
  if (query.ids.empty()) {
    return std::nullopt;
  }
  return query;
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
  UpdateProductRequest update{id, std::move(name), price_cents, expected_version};
  if (!valid_update_request(update)) {
    return std::nullopt;
  }
  return update;
}

void handle_get_result(httplib::Response& response, const GetProductResult& result) {
  response.set_header("X-Cache", cache_source_name(result.cache_source));
  if (result.status != ProductStatus::Ok) {
    set_product_status_error(response, result.status);
    return;
  }
  if (!result.product || !valid_product(*result.product)) {
    set_internal_error(response);
    return;
  }
  set_json_response(response, 200, product_json(*result.product).dump());
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
    if (item.result.status == ProductStatus::Ok && item.result.product &&
        valid_product(*item.result.product) && item.result.product->id == item.id) {
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

void handle_update_result(httplib::Response& response, const UpdateProductResult& result,
                          std::uint64_t id) {
  if (result.cache_invalidation_failed) {
    response.set_header("X-Cache-Invalidation", "failed");
    std::cerr << "product cache invalidation failed id=" << id << '\n';
  }
  if (result.status != ProductStatus::Ok) {
    set_product_status_error(response, result.status);
    return;
  }
  if (!result.product || !valid_product(*result.product) || result.product->id != id) {
    set_internal_error(response);
    return;
  }
  set_json_response(response, 200, product_json(*result.product).dump());
}

bool is_product_path(std::string_view path) noexcept {
  return path == "/products" || path.substr(0, sizeof("/products/") - 1) == "/products/";
}

const char* cache_backend_name(CacheBackend backend) noexcept {
  return backend == CacheBackend::Redis ? "redis" : "sphinx";
}

const char* cache_policy_name(CachePolicyMode policy) noexcept {
  return policy == CachePolicyMode::Protected ? "protected" : "basic";
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
                           CachePolicyMode policy_mode) {
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
  return Json{{"backend", cache_backend_name(backend)},
              {"policy", cache_policy_name(policy_mode)},
              {"counters", std::move(counters)},
              {"read_coordinator",
               Json{{"active_keys", reads_active_keys}, {"active_loads", reads_active_loads}}},
              {"cache_breaker", Json{{"state", cache_breaker_state_name(breaker.state)},
                                     {"consecutive_failures", breaker.consecutive_failures},
                                     {"retry_after_milliseconds", breaker.retry_after.count()}}}};
}

}  // namespace

void install_product_routes(httplib::Server& server,
                            const std::function<ProductService&()>& current_service,
                            const ProductSharedState& shared, CacheBackend backend,
                            CachePolicyMode policy_mode) {
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

  server.Get("/metrics", [&shared, backend, policy_mode](const httplib::Request&,
                                                         httplib::Response& response) {
    set_json_response(response, 200, make_metrics_response(shared, backend, policy_mode).dump());
  });

  server.Get("/products",
             [current_service](const httplib::Request& request, httplib::Response& response) {
               try {
                 const auto query = parse_product_batch_query(request);
                 if (!query) {
                   response.set_header("X-Cache", "NOT_CHECKED");
                   set_error_response(response, 400, "invalid_argument");
                   return;
                 }
                 handle_get_products_result(
                     response, current_service().get_many(query->ids, query->bypass_cache));
               } catch (...) {
                 response.set_header("X-Cache", "NOT_CHECKED");
                 set_internal_error(response);
               }
             });

  server.Get(R"(/products/([^/]+))",
             [current_service](const httplib::Request& request, httplib::Response& response) {
               // HTTP 层只解析请求并映射响应；缓存命中、回源和结果分类由 ProductService 决定。
               try {
                 const auto id = parse_product_id(request);
                 if (!id) {
                   response.set_header("X-Cache", "NOT_CHECKED");
                   set_error_response(response, 400, "invalid_argument");
                   return;
                 }
                 const auto fresh = parse_fresh_query(request);
                 if (!fresh) {
                   response.set_header("X-Cache", "NOT_CHECKED");
                   set_error_response(response, 400, "invalid_argument");
                   return;
                 }
                 handle_get_result(response, current_service().get(*id, *fresh));
               } catch (...) {
                 response.set_header("X-Cache", "NOT_CHECKED");
                 set_internal_error(response);
               }
             });

  server.Put(R"(/products/([^/]+))",
             [current_service](const httplib::Request& request, httplib::Response& response) {
               // 更新请求携带 expected_version，交给业务层和 MySQL 事务完成并发检查。
               try {
                 const auto id = parse_product_id(request);
                 if (!id) {
                   set_error_response(response, 400, "invalid_argument");
                   return;
                 }
                 if (!is_json_content_type(request)) {
                   set_error_response(response, 415, "unsupported_media_type");
                   return;
                 }
                 if (request.body.size() > 65536) {
                   set_error_response(response, 413, "payload_too_large");
                   return;
                 }
                 const auto update = parse_update_request(request, *id);
                 if (!update) {
                   set_error_response(response, 400, "invalid_argument");
                   return;
                 }
                 handle_update_result(response, current_service().update(*update), *id);
               } catch (...) {
                 set_internal_error(response);
               }
             });
}

}  // namespace sphinx
