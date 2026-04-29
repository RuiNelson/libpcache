## pcache_repl

A read-eval-print loop (REPL) utility for interacting with libpcache volumes from the command line. It is built as a C++ executable that dynamically links against `libpcache` and uses the [Taywee/args](https://github.com/Taywee/args) library for argument parsing.

### Usage

```sh
pcache_repl [--help] <command> [options]

Commands:
  create   Create a new volume
  open     Open an existing volume
```

### Subcommands

#### `create`

Creates a new volume on the filesystem.

```sh
pcache_repl create [-d DB_PATH] [-D DATA_PATH]
                   [--page-size PAGE_SIZE] [--max-pages MAX_PAGES]
                   [--policy POLICY] [--preallocate-database]
                   [--preallocate-datafile] [-s|--durable]
```

| Option                  | Description                                      | Default        |
| ----------------------- | ------------------------------------------------ | -------------- |
| `-d`, `--db`            | Path to the SQLite index database               | mandatory      |
| `-D`, `--data`          | Path to the data file                           | mandatory      |
| `--page-size`           | Page size in bytes                              | `4096`         |
| `--max-pages`           | Maximum number of pages                         | `1024`         |
| `--policy`              | Capacity policy: `fixed` or `fifo`              | `fixed`        |
| `--preallocate-database`| Preallocate database rows at creation           | `false`        |
| `--preallocate-datafile`| Preallocate data file at creation               | `false`        |
| `-s`, `--durable`       | Wait for `fsync` on each write                  | `false`        |
| `-h`, `--help`          | Show help for the create command                 |                |

#### `open`

Opens an existing volume and enters the REPL. Optionally executes commands from a script file.

```sh
pcache_repl open [-d DB_PATH] [-D DATA_PATH]
                [-c FILE|--commands FILE]
```

| Option                  | Description                                      | Default        |
| ----------------------- | ------------------------------------------------ | -------------- |
| `-d`, `--db`            | Path to the SQLite index database               | mandatory      |
| `-D`, `--data`          | Path to the data file                           | mandatory      |
| `-c`, `--commands`      | Path to a text file containing REPL commands to execute (silent mode, exit on error) | none |
| `-h`, `--help`          | Show help for the open command                   |                |

When `-c` / `--commands` is provided, the REPL reads and executes commands from the specified file instead of interactive mode. The program exits with status `1` if any command fails. If the file cannot be read, an error message is printed and the program exits with status `1`. This option is only available on the `open` subcommand.

### REPL Mode

When the REPL is running interactively, it reads commands from standard input. The prompt shows the current volume state and the available commands:

```
pcache_repl> help
```

### REPL Commands

The following commands are available inside the REPL:

| Command       | Description                                                      |
| ------------- | ---------------------------------------------------------------- |
| `help`        | Print the list of available commands                             |
| `put <ID_FILE> <PAGE_FILE>` | Store a page from a file; `ID_FILE` provides the identifier bytes and `PAGE_FILE` provides the page data |
| `get <ID_FILE>`            | Retrieve a page and print its contents to stdout                |
| `check <ID_FILE>`          | Check whether a page exists; prints `found` or `not found`       |
| `delete <ID_FILE>`          | Delete a page by identifier                                      |
| `pages`                    | Print used and free page counts                                 |
| `defragment [-s|--shrink]`  | Defragment the volume; optionally shrink the data file            |
| `close`                    | Close the volume and exit the REPL                              |

Identifiers and page data are always read from files (no inline buffer input). The identifier file must be exactly `id_size` bytes; the page file must be exactly `page_size` bytes.

Errors are reported in English. SQLite errors display the message from `sqlite3_errmsg()`. POSIX errors display the numeric error code.