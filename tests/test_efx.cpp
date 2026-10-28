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

    // Line with org2fromTrace (0x48c850): B = A + (A + row0 * 16384 - A) * fraction.
    {
        auto lineFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(
            "Line\n{\n\tspawnFlags org2fromTrace\n\tlife 1000\n\tshaders\n\t[\n\t\tgfx_x\n\t]\n}\n"));
        iw2::game::FxSystem ls;
        ls.trace = [](const iw2::game::FVec&, const iw2::game::FVec&, const iw2::game::FVec&, const iw2::game::FVec&, float& frac, iw2::game::FVec& n) { frac = 0.25f; n = {0, 0, 1}; return true; };
        ls.viewValid = true;
        ls.viewOrigin = {0, 500, 0};
        ls.play(lineFx, {10, 0, 0}, {1, 0, 0}, 0);
        const auto sp = ls.sprites(100);
        CHECK(sp.size() == 1);
        if (sp.size() == 1) CHECK(sp[0].beamTail && std::fabs(sp[0].beamB[0] - (10.0f + 4096.0f)) < 1e-2f && sp[0].beamA[0] == 10.0f);
    }

    // Cylinder (0x483450 / DLL 0x100401f0): B = A + length * row0, radii size2 (A) / size (B), segments from the distance.
    {
        auto cylFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(
            "Cylinder\n{\n\tlife 1000\n\tsize\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t]\n\t\tscale 5\n\t}\n"
            "\tlength\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t]\n\t\tscale 100\n\t}\n\tshaders\n\t[\n\t\tgfx_x\n\t]\n}\n"));
        iw2::game::FxSystem cs;
        cs.viewValid = true;
        cs.viewOrigin = {0, 0, 0};
        cs.play(cylFx, {0, 0, 0}, {0, 0, 1}, 0);
        const auto sp = cs.sprites(100);
        CHECK(sp.size() == 1);
        if (sp.size() == 1) {
            CHECK(sp[0].cylinder && sp[0].widthA == 5.0f && sp[0].widthB == 5.0f);
            CHECK(std::fabs(sp[0].beamB[2] - 100.0f) < 1e-4f);
            CHECK(sp[0].segments == 30); // round((1 - 50 / 1024) * 32) = round(30.44)
        }
    }

    // Particle angle (0x48ce70 start from `rotation`, 0x481b90 += frame ms * rotationDelta * 0.01) and half height = size2.
    {
        auto rotFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(
            "Particle\n{\n\tlife 1000\n\trotation 30\n\tnonUniformScale 1\n"
            "\trotationDelta\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t]\n\t\tscale 10\n\t}\n"
            "\tsize2\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 3\n\t\t]\n\t}\n}\n"));
        iw2::game::FxSystem rs;
        rs.play(rotFx, {0, 0, 0}, {1, 0, 0}, 0);
        const auto sp = rs.sprites(500);
        CHECK(sp.size() == 1);
        if (sp.size() == 1) CHECK(std::fabs(sp[0].rotation - 80.0f) < 1e-3f && sp[0].size2 == 3.0f);
    }

    // Flag tables (0x56d528): spawnFlags rows 24..36 also set element bits; a name outside the key's rows fails the effect.
    {
        const auto ff = iw2::assets::parseEfx("Particle\n{\n\tspawnFlags absoluteAccel evenDistribution\n}\n");
        CHECK(ff.elements.at(0).flagBits() == 0x400000u && ff.elements.at(0).spawnFlagBits() == 0x200u);
        bool threw = false;
        try { iw2::assets::parseEfx("Particle\n{\n\tspawnFlags rgbComponentInterpolation\n}\n"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        threw = false;
        try { iw2::assets::parseEfx("Particle\n{\n\tflags absoluteAccel\n}\n"); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
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
    auto bolt = std::make_shared<iw2::game::FxBolt>([&](iw2::game::FxTag& o) {
        o.origin = {tagX, 0, 0};
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
    // Tag rotation (0x48c290 end -> tag space, 0x47fcd0 / 0x4404f0 back through the current tag): a relative particle spawned
    // 10 units along the tag's forward follows the tag when it turns 90 degrees about z.
    {
        const char* relOff = "Particle\n{\n\tflags relative blocksSight\n\tlife 1000\n\torigin 10 0 0 10 0 0\n}\n";
        auto relFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(relOff));
        iw2::game::FxTag tag;
        tag.origin = {0, 0, 0};
        tag.hasAxes = true;
        auto rb = std::make_shared<iw2::game::FxBolt>([&](iw2::game::FxTag& o) { o = tag; return true; });
        iw2::game::FxSystem sr;
        sr.playBolted(relFx, rb, iw2::game::FxAxes{{{0, 0, 1}, {1, 0, 0}, {0, 1, 0}}}, 0);
        tag.axes = {iw2::game::FVec{0, 1, 0}, iw2::game::FVec{-1, 0, 0}, iw2::game::FVec{0, 0, 1}};
        sr.sprites(100);
        const auto& bl = sr.sightBlockers();
        CHECK(bl.size() == 1);
        if (bl.size() == 1) CHECK(std::fabs(bl[0].origin[0]) < 1e-4f && std::fabs(bl[0].origin[1] - 10.0f) < 1e-4f);
    }

    // Emitter physics step 0x480bd0 / 0x480a70: impactFx plays at the hit point, impactKills ends the element; a slow hit on an
    // upward normal comes to rest at the hit point; emitFx 0x483810 emits every `density` units along the ballistic path.
    {
        const char* vel = "\tvelocityX\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t\t1 1\n\t\t]\n\t\tscale 100\n\t}\n";
        auto killFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(
            std::string("Emitter\n{\n\tflags usePhysics impactKills impactFx\n\tlife 1000\n\timpactfx\n\t[\n\t\t/fx/hit\n\t]\n") + vel + "}\n"));
        std::vector<std::string> loads;
        iw2::game::FxSystem ks;
        ks.loader = [&](const std::string& n) { loads.push_back(n); return std::shared_ptr<const iw2::assets::EfxEffect>(); };
        ks.trace = [](const iw2::game::FVec&, const iw2::game::FVec&, const iw2::game::FVec&, const iw2::game::FVec&, float& frac,
                      iw2::game::FVec& n) { frac = 0.5f; n = {-1, 0, 0}; return true; };
        ks.play(killFx, {0, 0, 0}, {1, 0, 0}, 0);
        ks.sprites(0);
        ks.sprites(100);
        CHECK(loads.size() == 1 && loads[0] == "fx/hit.efx");
        ks.sprites(150);
        CHECK(ks.count() == 0);

        auto restFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(
            "Emitter\n{\n\tflags usePhysics\n\tlife 1000\n\tvelocityX\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t\t1 1\n\t\t]\n\t\tscale 3\n\t}\n}\n"));
        iw2::game::FxSystem rs;
        rs.trace = [](const iw2::game::FVec& a, const iw2::game::FVec&, const iw2::game::FVec&, const iw2::game::FVec&, float& frac,
                      iw2::game::FVec& n) { if (a[0] > 0.1f) return false; frac = 0.5f; n = {0, 0, 1}; return true; };
        rs.play(restFx, {0, 0, 0}, {1, 0, 0}, 0);
        rs.sprites(0);
        rs.sprites(100); // 3 · 0.1 = 0.3 units, hit at half: rest at 0.15 (speed² 9 < 16)
        rs.sprites(300);
        CHECK(rs.count() == 1);

        auto emitFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(
            std::string("Emitter\n{\n\tflags emitFx\n\tlife 1000\n\tdensity 10\n\temitfx\n\t[\n\t\tfx/trail\n\t]\n") + vel + "}\n"));
        std::vector<std::string> emits;
        iw2::game::FxSystem es;
        es.loader = [&](const std::string& n) { emits.push_back(n); return std::shared_ptr<const iw2::assets::EfxEffect>(); };
        es.play(emitFx, {0, 0, 0}, {1, 0, 0}, 0);
        es.sprites(0);
        es.sprites(100); // 12 ms steps: |V · ka| = 100 · 0.108 >= 10 at k = 108, backed off to k = 100
        CHECK(emits.size() == 1);
        es.sprites(200);
        CHECK(emits.size() == 2 && emits[1] == "fx/trail.efx");
    }
    // Bug 12: a relative Tail (view muzzle flash) whose tag turns keeps pointing along the tag (both ends through the bolt).
    {
        const char* tailTxt = "Tail\n{\n\tflags relative\n\tlife 1000\n\torigin 29 0 0\n"
                              "\tvelocityX\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t]\n\t\tscale 100\n\t}\n"
                              "\tlength\n\t{\n\t\tcurve\n\t\t[\n\t\t\t0 1\n\t\t]\n\t\tscale 20\n\t}\n\tshaders\n\t[\n\t\tgfx_x\n\t]\n}\n";
        auto tailFx = std::make_shared<iw2::assets::EfxEffect>(iw2::assets::parseEfx(tailTxt));
        iw2::game::FxTag tag;
        tag.hasAxes = true;
        auto tb = std::make_shared<iw2::game::FxBolt>([&](iw2::game::FxTag& o) { o = tag; return true; });
        iw2::game::FxSystem st;
        st.viewValid = true;
        st.viewOrigin = {0, 0, 50};
        st.playBolted(tailFx, tb, iw2::game::FxAxes{}, 0);
        st.sprites(50);
        tag.axes = {iw2::game::FVec{0, 1, 0}, iw2::game::FVec{-1, 0, 0}, iw2::game::FVec{0, 0, 1}}; // turned 90 degrees
        const auto sp = st.sprites(100);
        CHECK(sp.size() == 1);
        if (sp.size() == 1) {
            const float dx = sp[0].beamB[0] - sp[0].beamA[0], dy = sp[0].beamB[1] - sp[0].beamA[1];
            CHECK(std::fabs(dx) < 1e-3f && dy < -19.0f); // B behind A along the turned forward (+y)
            CHECK(std::fabs(sp[0].beamA[0]) < 1e-3f && sp[0].beamA[1] > 29.0f);
        }
    }
    if (failures) return 1;
    std::cout << "efx tests passed\n";
    return 0;
}
