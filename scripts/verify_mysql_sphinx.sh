#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run the real database and HTTP integration suites; missing credentials are an error, not a skip.
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
mysql_test="$build_dir/examples/mysql_sphinx/sphinx_mysql_product_integration_tests"
http_service="$build_dir/examples/mysql_sphinx/sphinx-product-service"
sphinx_server="$build_dir/sphinxd/sphinxd"
for executable in "$mysql_test" "$http_service" "$sphinx_server"; do
    if [[ ! -x $executable ]]; then
        echo "Missing $executable; configure with -DBUILD_MYSQL_SPHINX_DEMO=ON and build." >&2
        exit 2
    fi
done

"$mysql_test" --gtest_filter=MySqlProductStoreIntegrationTest.*
python3 examples/mysql_sphinx/test/product_http_integration_test.py "$sphinx_server" "$http_service"
