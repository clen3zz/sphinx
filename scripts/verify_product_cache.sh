#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 严格验证 Sphinx/Redis 与 basic/protected 四种组合；不允许依赖缺失时跳过。
set -euo pipefail

for name in SPHINX_TEST_MYSQL_HOST SPHINX_TEST_MYSQL_PORT SPHINX_TEST_MYSQL_USER \
            SPHINX_TEST_MYSQL_PASSWORD SPHINX_TEST_MYSQL_DATABASE; do
    if [[ -z ${!name:-} ]]; then
        echo "Missing $name; provide a disposable MySQL test database." >&2
        exit 2
    fi
done

database_lower=${SPHINX_TEST_MYSQL_DATABASE,,}
if [[ $database_lower != *test* ]]; then
    echo "SPHINX_TEST_MYSQL_DATABASE must contain 'test'." >&2
    exit 2
fi

build_dir=${1:-build}
mysql_test="$build_dir/product-service/sphinx_mysql_product_integration_tests"
http_service="$build_dir/product-service/sphinx-product-service"
sphinx_server="$build_dir/sphinxd/sphinxd"
http_test="product-service/test/product_http_integration_test.py"
for executable in "$mysql_test" "$http_service" "$sphinx_server"; do
    if [[ ! -x $executable ]]; then
        echo "Missing $executable; build the default product-service targets first." >&2
        exit 2
    fi
done

redis_server=${SPHINX_TEST_REDIS_SERVER:-}
if [[ -z $redis_server ]]; then
    redis_server=$(command -v redis-server || true)
fi
if [[ -z $redis_server || ! -x $redis_server ]]; then
    echo "redis-server is required for strict Redis acceptance." >&2
    exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required for HTTP acceptance." >&2
    exit 2
fi
if ! command -v mysql >/dev/null 2>&1; then
    echo "mysql client is required for HTTP fixture setup." >&2
    exit 2
fi

"$mysql_test" --gtest_filter=MySqlProductStoreIntegrationTest.*

for backend in sphinx redis; do
    for policy in basic protected; do
        echo "Running HTTP acceptance: backend=$backend policy=$policy"
        SPHINX_TEST_CACHE_BACKEND="$backend" \
            SPHINX_TEST_CACHE_POLICY="$policy" \
            SPHINX_TEST_REDIS_SERVER="$redis_server" \
            python3 "$http_test" "$sphinx_server" "$http_service"
    done
done
