// SPDX-License-Identifier: Apache-2.0
#include "product_http_routes.h"

#include <sphinx/product.h>

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

void set_product_status_error(httplib::Response& response, ProductStatus status) {
  switch (status) {
    case ProductStatus::InvalidArgument:
      set_error_response(response, 400, "invalid_argument");
      return;
    case ProductStatus::NotFound:
      set_error_response(response, 404, "not_found");
      return;
    case ProductStatus::Conflict:
      set_error_response(response, 409, "conflict");
      return;
    case ProductStatus::StoreUnavailable:
      set_error_response(response, 503, "store_unavailable");
      return;
    case ProductStatus::CommitUnknown:
      set_error_response(response, 503, "commit_unknown");
      return;
    case ProductStatus::Ok:
    case ProductStatus::InternalError:
      set_internal_error(response);
      return;
  }
  set_internal_error(response);
}

void set_exception_response(httplib::Response& response, const std::exception_ptr& exception) {
  try {
    if (exception) {
      std::rethrow_exception(exception);
    }
  } catch (const StoreError& error) {
    switch (error.code()) {
      case StoreErrorCode::Unavailable:
        set_error_response(response, 503, "store_unavailable");
        return;
      case StoreErrorCode::CommitUnknown:
        set_error_response(response, 503, "commit_unknown");
        return;
      case StoreErrorCode::InvalidData:
      case StoreErrorCode::Unexpected:
        set_internal_error(response);
        return;
    }
  } catch (...) {
    set_internal_error(response);
    return;
  }
  set_internal_error(response);
}

void set_product_response(httplib::Response& response, const Product& product) {
  Json body;
  body["id"] = product.id;
  body["name"] = product.name;
  body["price_cents"] = product.price_cents;
  body["version"] = product.version;
  set_json_response(response, 200, body.dump());
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

const char* product_status_error_name(ProductStatus status) noexcept {
  switch (status) {
    case ProductStatus::InvalidArgument:
      return "invalid_argument";
    case ProductStatus::NotFound:
      return "not_found";
    case ProductStatus::Conflict:
      return "conflict";
    case ProductStatus::StoreUnavailable:
      return "store_unavailable";
    case ProductStatus::CommitUnknown:
      return "commit_unknown";
    case ProductStatus::Ok:
    case ProductStatus::InternalError:
      return "internal_error";
  }
  return "internal_error";
}

std::optional<std::uint64_t> parse_product_id(const httplib::Request& request) {
  if (request.matches.size() < 2) {
    return std::nullopt;
  }
  const std::string text = request.matches[1].str();
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
  bool ids_seen = false;
  bool fresh_seen = false;
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
      if (ids_seen || value.empty()) {
        return std::nullopt;
      }
      ids_seen = true;
      std::size_t id_start = 0;
      while (id_start <= value.size()) {
        const auto comma = value.find(',', id_start);
        const auto id_end = comma == std::string_view::npos ? value.size() : comma;
        const auto text = value.substr(id_start, id_end - id_start);
        if (text.empty() || query.ids.size() >= max_product_batch_size) {
          return std::nullopt;
        }
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id, 10);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || id == 0) {
          return std::nullopt;
        }
        query.ids.push_back(id);
        if (comma == std::string_view::npos) {
          break;
        }
        id_start = comma + 1;
      }
    } else if (name == "fresh") {
      if (fresh_seen || value != "1") {
        return std::nullopt;
      }
      fresh_seen = true;
      query.bypass_cache = true;
    } else {
      return std::nullopt;
    }

    if (separator == std::string_view::npos) {
      break;
    }
    token_start = separator + 1;
  }
  if (!ids_seen || query.ids.empty()) {
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
  set_product_response(response, *result.product);
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
      value["error"] = product_status_error_name(item.result.status);
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
  set_product_response(response, *result.product);
}

bool is_product_path(std::string_view path) noexcept {
  return path == "/products" || path.substr(0, sizeof("/products/") - 1) == "/products/";
}

}  // namespace

void install_product_routes(httplib::Server& server,
                            const std::function<ProductService&()>& current_service) {
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
                 set_exception_response(response, std::current_exception());
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
                 set_exception_response(response, std::current_exception());
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
                 set_exception_response(response, std::current_exception());
               }
             });
}

}  // namespace sphinx
