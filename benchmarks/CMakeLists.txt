add_executable(sphinx_cache_path_benchmark cache_path_benchmark.cpp)
target_link_libraries(sphinx_cache_path_benchmark PRIVATE sphinx_product_redis
        sphinx_product_sphinx sphinx_httplib sphinx_json pthread)
