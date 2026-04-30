#include "errors.hxx"
#include <cerrno>
#include <cstring>
#include <iostream>
#include <libpcache_errors.h>
#include <map>
#include <string>

static const std::map<int, std::string> pcache_errors = {
    {PCACHE_CODE_IO_ERROR, "I/O error - a read or write operation failed"},
    {PCACHE_CODE_OUT_OF_MEMORY, "out of memory - could not allocate memory"},
    {PCACHE_CODE_INVALID_HANDLE, "invalid handle - the volume is not open"},
    {PCACHE_CODE_SQLITE_ERROR, "SQLite error - see SQLite error code below"},
    {PCACHE_CODE_CREATE_INVALID_ARGUMENT, "create: a required argument is NULL or zero"},
    {PCACHE_CODE_CREATE_FILE_EXISTS, "create: one or both volume files already exist"},
    {PCACHE_CODE_OPEN_NOT_FOUND, "open: one or both volume files do not exist"},
    {PCACHE_CODE_OPEN_CORRUPT, "open: the index database is missing required metadata"},
    {PCACHE_CODE_OPEN_SCHEMA_VERSION_TOO_HIGH, "open: database schema version is newer than supported"},
    {PCACHE_CODE_PUT_PAGE_CAPACITY_EXCEEDED, "put: FIXED volume is full and has no free slots"},
    {PCACHE_CODE_PUT_DUPLICATE_ID, "put: a page with this identifier already exists"},
    {PCACHE_CODE_PUT_INVALID_ARGUMENT,
     "put: position is out of bounds, counter would overflow, or endianness is invalid"},
    {PCACHE_CODE_GET_PAGE_NOT_FOUND, "get: no page with this identifier exists in the volume"},
    {PCACHE_CODE_GET_PAGES_NOT_FOUND, "get: no pages with these identifiers exist in the volume"},
    {PCACHE_CODE_GET_INVALID_ARGUMENT,
     "get: position is out of bounds, counter would overflow, or endianness is invalid"},
    {PCACHE_CODE_GET_RANGE_INVALID_RANGE, "get: 'first' is greater than 'last'"},
    {PCACHE_CODE_GET_RANGE_BUFFER_TOO_SMALL, "get: buffer capacity is smaller than the number of matching pages"},
    {PCACHE_CODE_CHECK_INVALID_ARGUMENT,
     "check: position is out of bounds, counter would overflow, or endianness is invalid"},
    {PCACHE_CODE_CHECK_RANGE_INVALID_RANGE, "check: 'first' is greater than 'last'"},
    {PCACHE_CODE_DELETE_INVALID_RANGE, "delete: 'first' is greater than 'last'"},
    {PCACHE_CODE_DELETE_INVALID_ARGUMENT,
     "delete: position is out of bounds, counter would overflow, or endianness is invalid"},
    {PCACHE_CODE_DEFRAGMENT_CANCELLED, "defragment: the progress callback returned false - volume remains consistent"},
    {PCACHE_CODE_SET_MAX_PAGES_WOULD_DISCARD_PAGES, "set_max_pages: FIXED volume has more live pages than new max"},
};

static std::string pcache_error_string(int pcache_err) {
    auto it = pcache_errors.find(pcache_err);
    return (it != pcache_errors.end()) ? it->second : "unknown";
}

bool print_error(int pcache_err, int sqlite_err, int posix_err) {
    if (pcache_err == 0 && sqlite_err == 0 && posix_err == 0) {
        return false;
    }

    if (pcache_err != 0) {
        std::cerr << "pcache error: 0x" << std::hex << pcache_err << std::dec << " " << pcache_error_string(pcache_err);

        if (pcache_err == PCACHE_CODE_SQLITE_ERROR && sqlite_err != 0) {
            std::cerr << " (SQLite error: " << sqlite_err << ")";
        } else if (pcache_err == PCACHE_CODE_IO_ERROR && posix_err != 0) {
            std::cerr << " (POSIX error: " << posix_err << ": " << strerror(posix_err) << ")";
        }
    } else {
        if (sqlite_err != 0) {
            std::cerr << "SQLite error: " << sqlite_err;
        }
        if (posix_err != 0) {
            if (sqlite_err != 0)
                std::cerr << " ";
            std::cerr << "POSIX error: " << posix_err << ": " << strerror(posix_err);
        }
    }

    std::cerr << std::endl;
    return true;
}

void print_ok() {
    std::cout << "OK" << std::endl;
}