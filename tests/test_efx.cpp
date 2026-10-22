// Tests for the effect text parser and the sight blockers of the effect runtime.

#include "assets/efx.h"
#include "game/fx.h"

#include <cmath>
#include <iostream>
#include <memory>

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

    // blocksSight particles become sight blockers: radius² from the size, visibility 1 − alpha byte / 255 with
    // useAlpha (alpha · 255 truncated) and 0 without it (0x481a40, 0x480970).
    const char* smoke =
        "Particle\n{\n\tname a\n\tflags blocksSight useAlpha\n\tlife 1000\n"
        "\talpha\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 0.5\n\t\t]\n\t}\n"
        "\tsize\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 3\n\t\t]\n\t}\n}\n"
        "Particle\n{\n\tname b\n\tflags blocksSight\n\tlife 1000\n}\n"
        "Particle\n{\n\tname c\n\tlife 1000\n}\n";
    auto smokeFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(smoke));
    iw2::game::FxSystem sys;
    sys.play(smokeFx, {10, 20, 30}, {1, 0, 0}, 0);
    sys.sprites(500);
    const auto& blockers = sys.sightBlockers();
    CHECK(blockers.size() == 2);
    if (blockers.size() == 2) {
        CHECK(blockers[0].origin[0] == 10 && blockers[0].origin[2] == 30);
        CHECK(blockers[0].radiusSq == 9);
        CHECK(std::fabs(blockers[0].visibility - (1.0f - 127.0f / 255.0f)) < 1e-6f);
        CHECK(std::fabs(blockers[1].visibility) < 1e-6f);
    }
    sys.sprites(2000);
    CHECK(sys.sightBlockers().empty());
    if (failures) return 1;
    std::cout << "efx tests passed\n";
    return 0;
}
