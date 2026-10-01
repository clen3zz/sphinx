#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# 运行真实数据库与 HTTP 集成测试；缺少凭据应报错，不能当作跳过测试。
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
for executable in "$mysql_test" "$http_service" "$sphinx_server"; do
    if [[ ! -x $executable ]]; then
        echo "Missing $executable; build the default MySQL product-service targets first." >&2
        exit 2
    fi
done

"$mysql_test" --gtest_filter=MySqlProductStoreIntegrationTest.*
python3 product-service/test/http/product_http_integration_test.py "$sphinx_server" "$http_service"
