# pcache_repl

Interactive REPL for [libpcache](https://github.com/rui/libpcache).

## Quick start

```bash
# Create a new volume
./build/repl/pcache_repl create /tmp/test.db /tmp/test.data

# Open an existing volume
./build/repl/pcache_repl open /tmp/test.db /tmp/test.data
```

## Commands

| Command | Description |
|---------|------------|
| `help` | List all commands |
| `put <id_file> <page_file> [--fail-if-exists] [--durable]` | Store a page |
| `get <id_file> <output_file>` | Retrieve a page to a file |
| `check <id_file>` | Check if a page exists |
| `delete <id_file> [--wipe] [--durable]` | Delete a page |
| `space` | Show used and free page counts |
| `inspect` | Show volume metadata and configuration |
| `set_max_pages <max_pages> [--durable]` | Update the maximum page limit |
| `defragment [--shrink] [--durable]` | Rearrange pages and optionally shrink the volume |
| `close` | Close the volume |

## Global flags

- `--durable` — waits for `fsync` before returning (guarantees persistence)
- `--wipe` — overwrites deleted data with zeros

## File format

- `<id_file>` — opaque binary content that identifies the page
- `<page_file>` — binary page content (fixed size, set at volume creation)

## Building

```bash
cmake -B build
cmake --build build
```

The binary is produced at `build/repl/pcache_repl`.