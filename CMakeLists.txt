cmake_minimum_required (VERSION 3.10)
project (HTTPSServer)

# LIBRESSL
set (LIBRESSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/libressl")

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/libressl/VERSION")
	message (STATUS "Autogen LibreSSL...")
	execute_process (
		COMMAND ./autogen.sh
		WORKING_DIRECTORY "${LIBRESSL_DIR}"
		RESULT_VARIABLE autogen_result
	)
	if (NOT autogen_result EQUAL 0)
		message (FATAL_ERROR "LibreSSL autogen failed")
	endif ()
endif ()

set (LIBRESSL_APPS OFF CACHE BOOL "" FORCE)
set (LIBRESSL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory ("${LIBRESSL_DIR}")

# LUAJIT
set (LUAJIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/luajit")
set (LUAJIT_LIB "${LUAJIT_DIR}/src/libluajit.a")

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	set (LUA_EXTRA_FLAGS "MACOSX_DEPLOYMENT_TARGET=11.0")
endif ()

if (NOT EXISTS "${LUAJIT_LIB}")
	message (STATUS "Building LuaJIT...")
	execute_process (
		COMMAND make ${LUA_EXTRA_FLAGS}
		WORKING_DIRECTORY "${LUAJIT_DIR}"
		RESULT_VARIABLE luajit_build_result
	)
	if (NOT luajit_build_result EQUAL 0)
		message (FATAL_ERROR "LuaJIT build failed")
	endif ()
endif ()
add_library (luajit_lib STATIC IMPORTED)

set_target_properties (luajit_lib PROPERTIES
	IMPORTED_LOCATION "${LUAJIT_LIB}"
)

# SQLLITE
set (SQLITE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/sqlite")
set (SQLITE_C "${SQLITE_DIR}/sqlite3.c")

if (NOT EXISTS ${SQLITE_C})
	message (STATUS "Building SQLite...")
	execute_process (
		COMMAND sh ./configure
		WORKING_DIRECTORY "${SQLITE_DIR}"
		RESULT_VARIABLE config_result
	)
	if (NOT config_result EQUAL 0)
		message (FATAL_ERROR "SQLite configuration failed")
	endif ()
	execute_process (
		COMMAND make sqlite3.c
		WORKING_DIRECTORY ${SQLITE_DIR}
		RESULT_VARIABLE sqlite_gen_result
	)
	if (NOT sqlite_gen_result EQUAL 0)
		message (FATAL_ERROR "SQLite build faield")
	endif ()
endif ()

if (NOT ${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	# target_link_options (server PRIVATE -static)
	# don't warn about executable stack:
	target_link_options (server PRIVATE "-Wl,-z,noexecstack")
endif ()

set (TARGET_NAME server) 

add_executable (${TARGET_NAME}
	"src/main.cpp"
	"src/server.cpp"
	"src/worker.cpp"
	"src/lua_bnd.cpp"
	"${SQLITE_DIR}/sqlite3.c"
)

target_compile_features(${TARGET_NAME} PRIVATE cxx_std_17)

target_include_directories (${TARGET_NAME} PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/src"
	"${LIBRESSL_DIR}/include/"
	"${LUAJIT_DIR}/src/"
	"${SQLITE_DIR}/"
)

target_link_libraries (${TARGET_NAME} PRIVATE
	m
	dl
	tls
	ssl
	crypto
	luajit_lib
)
