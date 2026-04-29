# pcache_repl

Interactive REPL for [libpcache](https://github.com/rui/libpcache). Built as a C++ executable that links against `libpcache` and uses [Taywee/args](https://github.com/Taywee/args) for argument parsing.

## Building

```bash
cmake -B build
cmake --build build
```

The binary is produced at `build/repl/pcache_repl`.

## Quick start

```bash
# Create a new volume
./build/repl/pcache_repl create -d /tmp/test.db -D /tmp/test.data

# Open an existing volume
./build/repl/pcache_repl open -d /tmp/test.db -D /tmp/test.data
```

## Subcommands

### `create`

Creates a new volume on the filesystem.

```
pcache_repl create [-d DB_PATH] [-D DATA_PATH]
                   [--page-size PAGE_SIZE] [--max-pages MAX_PAGES]
                   [--policy POLICY] [--preallocate-database]
                   [--preallocate-datafile] [-s|--durable]
```

| Option | Description | Default |
|---|---|---|
| `-d`, `--db` | Path to the SQLite index database | mandatory |
| `-D`, `--data` | Path to the data file | mandatory |
| `--page-size` | Page size in bytes | `4096` |
| `--max-pages` | Maximum number of pages | `1024` |
| `--policy` | Capacity policy: `fixed` or `fifo` | `fixed` |
| `--preallocate-database` | Preallocate database rows at creation | `false` |
| `--preallocate-datafile` | Preallocate data file at creation | `false` |
| `-s`, `--durable` | Wait for `fsync` on each write | `false` |

### `open`

Opens an existing volume and enters the REPL. Optionally executes commands from a script file.

```
pcache_repl open [-d DB_PATH] [-D DATA_PATH]
                [-c FILE|--commands FILE]
```

| Option | Description | Default |
|---|---|---|
| `-d`, `--db` | Path to the SQLite index database | mandatory |
| `-D`, `--data` | Path to the data file | mandatory |
| `-c`, `--commands` | Path to a text file containing REPL commands to execute (silent mode, exits on error) | none |

When `-c` / `--commands` is provided, the REPL reads and executes commands from the specified file instead of interactive mode. The program exits with status `1` if any command fails. If the file cannot be read, an error message is printed and the program exits with status `1`.

## REPL commands

Inside the REPL, the following commands are available:

| Command | Description |
|---|---|
| `help` | List all commands |
| `put <id_file> <page_file> [--fail-if-exists] [--durable]` | Store a page |
| `get <id_file> <output_file>` | Retrieve a page to a file |
| `check <id_file>` | Check if a page exists; prints `found` or `not found` |
| `delete <id_file> [--wipe] [--durable]` | Delete a page |
| `space` | Show used and free page counts |
| `inspect` | Show volume metadata and configuration |
| `set_max_pages <max_pages> [--durable]` | Update the maximum page limit |
| `defragment [--shrink] [--durable]` | Rearrange pages and optionally shrink the volume |
| `close` | Close the volume and exit the REPL |

### Command details

#### `put <id_file> <page_file> [--fail-if-exists] [--durable]`

Stores a page in the volume. Both arguments are file paths:
- `<id_file>` — binary content that identifies the page (must be exactly `id_size` bytes)
- `<page_file>` — binary page content (must be exactly `page_size` bytes)

Flags:
- `--fail-if-exists` — if a page with the same identifier already exists, return an error instead of overwriting
- `--durable` — waits for `fsync` before returning

#### `get <id_file> <output_file>`

Retrieves the page identified by `<id_file>` and writes it to `<output_file>`. Prints the page size on success.

#### `check <id_file>`

Checks whether a page with the given identifier exists. Prints `found` or `not found`.

#### `delete <id_file> [--wipe] [--durable]`

Deletes the page with the given identifier.

Flags:
- `--wipe` — overwrites the page data with zeros before removing the index entry
- `--durable` — waits for `fsync` before returning

#### `space`

Shows the number of used and free pages in the volume. Output format: `Used: <count>, Free: <count>`.

#### `inspect`

Displays the volume's current configuration and metadata:
- Page size
- Maximum pages
- ID size
- Capacity policy
- Schema version

#### `set_max_pages <max_pages> [--durable]`

Updates the maximum number of pages the volume can hold.

Flags:
- `--durable` — waits for `fsync` before returning

For `FIXED` volumes, shrinking below the current live page count triggers automatic relocation of endangered pages into free slots.

#### `defragment [--shrink] [--durable]`

Rearranges live pages to fill gaps left by deletions, restoring sequential layout.

Flags:
- `--shrink` — after defragmentation, truncates the data file to remove unused space at the end
- `--durable` — waits for `fsync` before returning

#### `close`

Closes the volume and exits the REPL. Both the data file and SQLite database are `fsync`'d before the handles are closed, ensuring all data is persisted.

## Global flags

These flags can be combined with the commands above:

- `--durable` — waits for `fsync` before returning (guarantees persistence on the data file)
- `--wipe` — overwrites deleted data with zeros

## File format

- `<id_file>` — opaque binary content that identifies the page (exact size determined by volume's `id_size`)
- `<page_file>` — binary page content (exact size determined by volume's `page_size`)

Identifiers and page data are always read from files — there is no inline buffer input.

## Error handling

Errors are reported in English. SQLite errors display the message from `sqlite3_errmsg()`. POSIX errors display the numeric error code.

## Interactive mode

When running without `-c`, the REPL enters interactive mode and displays:

```
pcache> <command>
```

Press `Ctrl+C` or `Ctrl+D` to exit cleanly. The REPL also maintains a command history (using `readline`) — use the up/down arrow keys to navigate previous commands.