// SPDX-License-Identifier: Apache-2.0
#include "product_http_routes.h"

#include <sphinx/product.h>

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

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

bool ascii_space(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' ||
         value == '\v';
}

std::string_view trim_ascii(std::string_view value) noexcept {
  while (!value.empty() && ascii_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && ascii_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

bool ascii_iequals(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    auto left = static_cast<unsigned char>(lhs[index]);
    auto right = static_cast<unsigned char>(rhs[index]);
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<unsigned char>(left + ('a' - 'A'));
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<unsigned char>(right + ('a' - 'A'));
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

bool is_json_content_type(const httplib::Request& request) {
  if (request.get_header_value_count("Content-Type") != 1) {
    return false;
  }
  const std::string value = request.get_header_value("Content-Type");
  const std::string_view content_type{value};
  const auto parameter_start = content_type.find(';');
  if (!ascii_iequals(trim_ascii(content_type.substr(0, parameter_start)), "application/json")) {
    return false;
  }
  if (parameter_start == std::string_view::npos) {
    return true;
  }
  const auto parameters = content_type.substr(parameter_start + 1);
  if (parameters.find(';') != std::string_view::npos) {
    return false;
  }
  const auto parameter = trim_ascii(parameters);
  const auto equals = parameter.find('=');
  if (equals == std::string_view::npos) {
    return false;
  }
  return ascii_iequals(trim_ascii(parameter.substr(0, equals)), "charset") &&
         ascii_iequals(trim_ascii(parameter.substr(equals + 1)), "utf-8");
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
  bool duplicate_key = false;
  std::unordered_set<std::string> keys;
  const auto callback = [&duplicate_key, &keys](int, Json::parse_event_t event,
                                                const Json& parsed) {
    if (event == Json::parse_event_t::key && !keys.insert(parsed.get<std::string>()).second) {
      duplicate_key = true;
      return false;
    }
    return true;
  };
  const Json body = Json::parse(request.body.begin(), request.body.end(), callback, false);
  if (duplicate_key || body.is_discarded() || !body.is_object() || body.size() != 3 ||
      !body.contains("name") || !body.contains("price_cents") ||
      !body.contains("expected_version")) {
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
      !parse_unsigned_integer(body.at("expected_version"), &expected_version) ||
      expected_version == 0 || expected_version == std::numeric_limits<std::uint64_t>::max() ||
      !valid_product(Product{id, name, price_cents, 1})) {
    return std::nullopt;
  }
  return UpdateProductRequest{id, std::move(name), price_cents, expected_version};
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

}  // 匿名命名空间

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

}  // 命名空间 sphinx
