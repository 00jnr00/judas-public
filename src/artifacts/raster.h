// Software renderers for artifact previews, compiled into judas (no browser, no helper program):
//   * STL and GLB meshes: z-buffer rasterizer, flat shading, key plus fill light, auto-framed orthographic cameras.
//   * SVG: nanosvg.
//   * PNG encoding: stb_image_write.
#pragma once

#include <string>
#include <vector>

namespace judas {

struct Mesh {
    std::vector<float> tri;   // 9 floats per triangle, Z-up world
    size_t triangles() const { return tri.size() / 9; }
};

struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;   // w*h*4
};

// Mesh loaders: false with err on bad input. Triangle cap protects memory (about 4 million).
bool load_stl(const std::string & bytes, Mesh & out, std::string & err);   // binary or ASCII
bool load_glb(const std::string & bytes, Mesh & out, std::string & err);   // GLB 2.0, triangle primitives, node transforms; Y-up becomes Z-up

// Views: "iso", "front", "back", "left", "right", "top", "bottom". More than one view are tiled in a grid
// (2 views side by side, 3 or 4 as 2x2). Orthographic, every tile framed to the mesh bounding sphere.
Image render_mesh(const Mesh & m, int w, int h, const std::vector<std::string> & views);
std::vector<std::string> default_views();   // iso, front, right, top

// Rasterizes an SVG document onto a white background, scaled to fit w x h (max edge); 0 w/h = natural size capped at 2048.
bool render_svg(const std::string & svg, int w, int h, Image & out, std::string & err);

std::string encode_png(const Image & img);
// Width and height from a PNG header; false when it is not a PNG.
bool png_size(const std::string & png, int & w, int & h);

}  // namespace judas
