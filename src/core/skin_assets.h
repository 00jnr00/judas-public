// Themes shipped in the repo's skins/ folder, embedded into the binary at build time (cmake/embed_skins.cmake).
#pragma once
#include <cstddef>
namespace judas {
struct SkinAsset {
    const char * file;  // e.g. "ember-plum-dark.toml"
    const char * text;  // the TOML source, NUL terminated
};
const SkinAsset * skin_assets(size_t & n);  // n entries
}
