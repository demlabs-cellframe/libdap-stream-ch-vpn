cmake_minimum_required(VERSION 3.0)
project (dap_stream_ch_vpn)
  
set(VPN_SRCS dap_stream_ch_vpn.c)

if(WIN32)
  include_directories(../libdap/src/win32/)
  include_directories(../3rdparty/libmemcached/)
  include_directories(../3rdparty/libmemcached/win32/)
  include_directories(../3rdparty/uthash/src/)
  include_directories(../3rdparty/libjson-c/)
  include_directories(../3rdparty/curl/include/)
  include_directories(../3rdparty/libsqlite3/)
  #include_directories(../sources/wepoll/)
endif()

add_library(${PROJECT_NAME} STATIC ${VPN_SRCS})

target_link_libraries(dap_stream_ch_vpn dap_core dap_crypto dap_stream)

target_include_directories(dap_stream_ch_vpn INTERFACE .)
