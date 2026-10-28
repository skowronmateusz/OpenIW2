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

    // Legacy channel syntax (0x487910 -> 0x47eb10): minefield.efx style `start` / `end` / `flags linear`.
    {
        const char* leg =
            "Line\n{\n\tlife 1422\n"
            "\talpha\n\t{\n\t\tend\t0\n\t\tflags\tlinear\n\t}\n"
            "\tsize\n\t{\n\t\tstart\t32 74\n\t\tend\t442 342\n\t\tflags\tlinear\n\t}\n"
            "\tlength\n\t{\n\t\tstart\t2\n\t\tend\t3\n\t}\n}\n";
        const auto lfx = iw2::assets::parseEfx(leg);
        const auto& le = lfx.elements.at(0);
        const auto& a = le.graphs.at("alpha"); // alpha: M = 1, start defaults to 1 -> rows {0, 1}, {1, 0}
        CHECK(a.points.size() == 2 && a.points[0][1] == 1.0f && a.points[1][1] == 0.0f);
        CHECK(a.scaleMin == 1.0f && a.scaleMax == 1.0f);
        const auto& sz = le.graphs.at("size"); // M = 392 * 4/3, scale M -/+ 21, values mid / M
        const float M = 392.0f * 1.3333334f;
        CHECK(sz.points.size() == 2);
        CHECK(std::fabs(sz.points[0][1] - 53.0f / M) < 1e-5f && std::fabs(sz.points[1][1] - 392.0f / M) < 1e-5f);
        CHECK(std::fabs(sz.scaleMin - (M - 21.0f)) < 1e-3f && std::fabs(sz.scaleMax - (M + 21.0f)) < 1e-3f);
        const auto& ln = le.graphs.at("length"); // no flags: 16 rows of the constant start value (f = 1)
        CHECK(ln.points.size() == 16 && std::fabs(ln.points[15][0] - 1.0f) < 1e-6f);
        CHECK(std::fabs(ln.points[7][1] * 4.0f - 2.0f) < 1e-5f); // M = max(1, 3 * 4/3) = 4
    }

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
    // Bolted effect (0x4853f0 attachment, 0x485bf0): a relative element follows the tag, a non-relative one stays; an
    // invalid attachment plays nothing.
    const char* bolted =
        "Particle\n{\n\tname rel\n\tflags relative blocksSight\n\tlife 1000\n\tsize\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 3\n\t\t]\n\t}\n}\n"
        "Particle\n{\n\tname abs\n\tflags blocksSight\n\tlife 1000\n\tsize\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 3\n\t\t]\n\t}\n}\n";
    auto boltedFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(bolted));
    iw2::game::FxSystem sys2;
    float tagX = 100;
    bool valid = true;
    auto bolt = std::make_shared<iw2::game::FxBolt>([&](iw2::game::FVec& o) {
        o = {tagX, 0, 0};
        return valid;
    });
    sys2.playBolted(boltedFx, bolt, iw2::game::FxAxes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, 0);
    tagX = 150;
    sys2.sprites(100);
    const auto& b2 = sys2.sightBlockers();
    CHECK(b2.size() == 2);
    if (b2.size() == 2) {
        CHECK(std::fabs(b2[0].origin[0] - 150) < 1e-3f); // relative: moved with the tag
        CHECK(std::fabs(b2[1].origin[0] - 100) < 1e-3f); // not relative: stays
    }
    valid = false;
    iw2::game::FxSystem sys3;
    sys3.playBolted(boltedFx, bolt, iw2::game::FxAxes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}, 0);
    sys3.sprites(100);
    CHECK(sys3.count() == 0);
    if (failures) return 1;
    std::cout << "efx tests passed\n";
    return 0;
}
