// Embedded web assets (generated at build time from web/). Served by the HTTP server so judas is one binary.
#pragma once
#include <cstddef>
namespace judas {
struct Asset {
    const char * path;          // served path, e.g. "/index.html"
    const char * mime;
    const unsigned char * data;
    size_t size;
};
const Asset * assets_table(size_t & n);  // n entries; empty table if web/ was empty at build time
}
