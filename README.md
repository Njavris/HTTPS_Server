# HTTPSServer

A lightweight HTTPS web server with Lua scripting support, SQLite database integration, TLS encryption, and a worker pool for concurrent request handling.

## Building

### Prerequisites

- CMake 3.10 or higher
- C++17 compatible compiler (GCC, clang, etc.)
- libssl-dev or equivalent (for `dl` and `m`)

### Steps

```
mkdir build && cd build && cmake .. && make
```

The build system includes LibreSSL, LuaJIT, and SQLite from `external/` subdirectories. Run `cmake` in the build directory then `make` to compile the `server` binary.

## Configuration

Create a config file (default: `cfg.ini`) with the following structure:

```
[server]
port          = 443
keep-alive    = 30
serve_file    = server.lua
sql_db_file   = db.sqlite

[ssl]
protocols     = all
cert_file     = /path/to/cert.pem
key_file      = /path/to/key.pem
```

### Settings

**[server]**
- `port` - HTTPS port to listen on (default: 443)
- `keep-alive` - Connection keep-alive timeout in seconds (default: 30)
- `serve_file` - Lua script to execute for request handling
- `sql_db_file` - SQLite database file path

**[ssl]**
- `protocols` - TLS protocol versions to support
- `cert_file` - TLS certificate file path
- `key_file` - TLS private key file path

Path values in `serve_file` and `sql_db_file` are resolved relative to the config file's directory. Absolute paths are kept as-is.

## Running

```
./server -C path/to/cfg.ini
```

- `-C` specifies the config file path (default: `./cfg.ini`)
- `SIGPIPE` is ignored to prevent crashes on half-open connections

## Lua API

All Lua functions are available inside the server script file specified by `server.serve_file`. Every request is handled in a dedicated Lua thread.

### db_exec(sql)

Execute a SQL statement (INSERT, UPDATE, DELETE, CREATE, etc.).

Arguments:
  sql (string) - SQL query string

Returns: (boolean success, string? error_message) - true on success, or false + error on failure.

Example:

  local ok, err = db_exec("INSERT INTO users (name) VALUES ('test')")
  if not ok then
      print("Error: " .. err)
  end

### db_query(sql)

Execute a SQL SELECT query. Returns an indexed table of rows, where each row is a table of column name to value mappings.

Arguments:

  sql (string) - SQL query string

Returns: table - An indexed table where each row is a table of column name to value strings.

Example:

  local rows = db_query("SELECT id, name FROM users")
  for i, row in ipairs(rows) do
      print(row.id .. ": " .. row.name)
  end

### send_response(fd, response_table)

Send an HTTP response back to the connected client.

Arguments:

  fd             (number)  - Client file descriptor
  response_table (table)   - Response parameters (see below)

response_table fields:

  status   (number, default 200)   - HTTP status code
  body     (string,  default "")   - Response body content
  headers  (table,   default {})   - Table of {"key": "value"} pairs

Returns: No return value.

Example:

  send_response(fd, {
      status = 200,
      body = "<h1>Hello</h1>",
      headers = {
          ["Content-Type"] = "text/html",
          ["X-Custom"] = "value",
      }
  })

### hash_password(password)

Hash a password with a random 16-byte salt and SHA-256.

Arguments:

  password (string) - The plain-text password to hash

Returns: string - "salt_hex:hash_hex" format.

Example:

  local hashed = hash_password("secretpass")
  -- hashed = "a1b2c3...:d4e5f6..."

### verify_password(password, stored_hash)

Verify a plain-text password against a previously stored hash entry.

Arguments:

  password    (string) - Plain-text password
  stored_hash (string) - Previously stored "salt:hash" entry

Returns: boolean - true if the password matches, false otherwise.

Example:

  local ok = verify_password("secretpass", hashed)
  -- ok is true if the password matches

### generate_token()

Generate a cryptographically secure random hex token (64 character string from 32 random bytes).

Arguments: None.

Returns: string - Random hex token.

Example:

  local token = generate_token()
  -- token = "a1b2c3d4... (64 chars)"

### save_file(filename, data)

Save binary data to a file in the "uploads/" directory.

Arguments:

  filename (string) - Name of the file to create
  data     (string) - Binary data to write

Returns: boolean - true on success, false on failure.

Example:

  local ok = save_file("upload.bin", file_data)
  if ok then
      print("File saved successfully")
  end

## Architecture

- **Server** - Single-threaded TCP listener (port binding and TLS setup).
- **WorkerPool** - Thread pool for processing requests with stuck detection (10 second threshold, logs to stderr).
- **SendQueue** - Mutex-protected queue for sending responses from Lua back to the main thread.
- **wakePipe** - Pipe for waking the main select loop when responses are enqueued.
- **thread_local Lua** - Each thread gets a dedicated Lua instance with a private database handle to avoid threading issues.
