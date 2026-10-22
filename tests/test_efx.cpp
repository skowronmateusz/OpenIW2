// Tests for the effect text parser.

#include "assets/efx.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
            ++failures;                                                             \
        }                                                                           \
    } while (0)
} // namespace

int main() {
    const char* text =
        "Particle\r\n{\r\n\tname\t\tglow\r\n\tflags\tuseAlpha relative\r\n\tlife 100 200\r\n"
        "\trgb\r\n\t{\r\n\t\tcurve\r\n\t\t[\r\n\t\t\t0 1 0.5 0.25\r\n\t\t\t1 0 0 0\r\n\t\t]\r\n\t}\r\n"
        "\tsize\r\n\t{\r\n\t\tcurve\r\n\t\t[\r\n\t\t\t0.5 2 9 9\r\n\t\t]\r\n\t\tscale 10 20\r\n\t}\r\n"
        "\tshaders\r\n\t[\r\n\t\tgfx_a\r\n\t\tgfx_b\r\n\t]\r\n}\r\n";
    const auto fx = iw2::assets::parseEfx(text);
    CHECK(fx.elements.size() == 1);
    const auto& e = fx.elements[0];
    CHECK(e.type == "Particle" && e.name == "glow");
    CHECK(e.flags().size() == 2 && e.flags()[1] == "relative");
    CHECK(e.numbers("life").size() == 2 && e.numbers("life")[1] == 200);
    CHECK(e.list("shaders").size() == 2 && e.list("shaders")[1] == "gfx_b");
    const auto& rgb = e.graphs.at("rgb");
    CHECK(rgb.dims == 3 && std::fabs(rgb.sample(0.5f, 1) - 0.25f) < 1e-5f);
    const auto& size = e.graphs.at("size");
    CHECK(size.points.size() == 3);                  // 0 and 1 added around the single row at 0.5
    CHECK(size.points[0][1] == 2 && size.points[2][1] == 2);
    CHECK(size.scaleMin == 10 && size.scaleMax == 20);
    if (failures) return 1;
    std::cout << "efx tests passed\n";
    return 0;
}
