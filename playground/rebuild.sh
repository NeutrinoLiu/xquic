rm -f test_* *log tp_localhost xqc_token
cd ../third_party/boringssl
SSL_TYPE_STR="boringssl"
SSL_PATH_STR="${PWD}"
SSL_INC_PATH_STR="${PWD}/include"
SSL_LIB_PATH_STR="${PWD}/build/ssl/libssl.a;${PWD}/build/crypto/libcrypto.a"
cd ../../build
cmake -DGCOV=on -DCMAKE_BUILD_TYPE=Debug -DXQC_ENABLE_TESTING=1 -DXQC_ENABLE_EVENT_LOG=1 -DXQC_ENABLE_RENO=1 -DSSL_TYPE=${SSL_TYPE_STR} -DSSL_PATH=${SSL_PATH_STR} -DSSL_INC_PATH=${SSL_INC_PATH_STR} -DSSL_LIB_PATH=${SSL_LIB_PATH_STR} ..
make -j
cp test_client ../playground/
cp test_server ../playground/
