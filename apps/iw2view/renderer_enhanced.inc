// The enhanced mode's out-of-line Renderer passes (docs/render-enhanced.md), included by main.cpp at this point; it is a
// part of main.cpp (same translation unit), split off to keep that file under the repository's size limit.
// ---- enhanced mode passes (docs/render-enhanced.md) ----

void Renderer::enhPrepare(SDL_GPUCommandBuffer* cmd, const Scene& scene, const Camera& cam, const std::vector<DrawItem>& items, uint32_t, uint32_t) { // sizes: the render size (the aspect is frameAspect())
    enhStats = {};
    EnhFragUC f{};
    sunDirForAO[0] = scene.sunDirection.x, sunDirForAO[1] = scene.sunDirection.y, sunDirForAO[2] = scene.sunDirection.z;
    f.eye[0] = cam.pos.x, f.eye[1] = cam.pos.y, f.eye[2] = cam.pos.z;
    shadowCascadesNow = 0;
    const game::SunShadowQuality q = game::sunShadowQuality(enh.sunShadows);
    const bool sunLit = scene.sunColor.x + scene.sunColor.y + scene.sunColor.z > 0.0f;
    worldCascadesNow = 0;
    const game::WorldShadowQuality wq = game::worldShadowQuality(enh.worldShadows);
    if (wq.cascades > 0 && sunLit && !scene.batches.empty()) {
        if (worldBoundsOf != scene.serial) { // the world's box: every soup and static model
            worldBoundsOf = scene.serial;
            for (int a = 0; a < 3; ++a) worldMins[a] = 1e30f, worldMaxs[a] = -1e30f;
            for (const auto& b : scene.soupBoxes)
                for (int a = 0; a < 3; ++a) worldMins[a] = std::min(worldMins[a], b[size_t(a)]), worldMaxs[a] = std::max(worldMaxs[a], b[size_t(a + 3)]);
            for (const StaticModelInstance& m : scene.staticModels)
                for (int a = 0; a < 3; ++a) worldMins[a] = std::min(worldMins[a], m.mins[a]), worldMaxs[a] = std::max(worldMaxs[a], m.maxs[a]);
        }
        const float sun[3] = {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z};
        const float camP[3] = {cam.pos.x, cam.pos.y, cam.pos.z};
        for (int c = 0; c < wq.cascades; ++c) {
            WorldCascade& wc = worldCascades[c];
            const auto centre = game::cascadeCentre(camP, wq.radius[c]);
            static const bool forceCascades = std::getenv("IW2_ENH_CASCADE_REDRAW") != nullptr; // debug: re-render every frame (cost of a move)
            if (!wc.valid || wc.sceneSerial != scene.serial || wc.centre != centre || forceCascades) {
                wc.centre = centre;
                wc.sceneSerial = scene.serial;
                renderWorldCascade(cmd, scene, c, wq, sun);
                wc.valid = true;
            }
            std::memcpy(f.worldVP[c], wc.vp, sizeof(f.worldVP[c]));
            f.worldSphere[c][0] = wc.centre[0], f.worldSphere[c][1] = wc.centre[1], f.worldSphere[c][2] = wc.centre[2], f.worldSphere[c][3] = wq.radius[c];
            f.worldBias[c] = wc.depthBias;
        }
        worldCascadesNow = wq.cascades;
        f.worldParams[0] = float(wq.cascades);
        f.worldParams[1] = 1.0f / float(wq.resolution);
        f.worldParams[2] = 1.5f;
        f.worldParams[3] = float(wq.pcfTaps);
    }
    if (shadowTex && q.resolution > 0 && sunLit && !(rtShadowNow && enh.rtShadows)) { // (ray-traced: the AO pass's sun channel)
        // Sun shadow map of the moving casters (docs/render-enhanced.md "Sun shadows"): actors, vehicles, script models and brush
        // movers. Static models and the world are in the baked sun mask already; the receivers multiply both.
        SDL_GPUDepthStencilTargetInfo dt{};
        dt.texture = shadowTex;
        dt.clear_depth = 1.0f;
        dt.load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* sp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt);
        SDL_BindGPUGraphicsPipeline(sp, enhShadowPipe);
        const float sun[3] = {scene.sunDirection.x, scene.sunDirection.y, scene.sunDirection.z};
        Vec3 fwd = cam.forward();
        fwd.z = 0.0f;
        const float fl = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y);
        fwd = fl > 1e-4f ? fwd * (1.0f / fl) : Vec3{};
        constexpr float kCasterReach = 3000.0f; // casters this far beyond the sphere towards the sun still land in the map
        struct SVU { float vp[16]; Mat4 model; float p[4]; };
        for (int c = 0; c < q.cascades; ++c) {
            const float R = q.radius[c];
            const Vec3 centre = cam.pos + fwd * (R * 0.4f); // the camera stays inside; most of the area is ahead
            const float cc[3] = {centre.x, centre.y, centre.z};
            const game::SunShadowFit fit = game::fitSunShadow(sun, cc, R, kCasterReach, q.resolution);
            std::memcpy(f.shadowVP[c], fit.viewProj.data(), sizeof(f.shadowVP[c]));
            f.shadowSphere[c][0] = centre.x, f.shadowSphere[c][1] = centre.y, f.shadowSphere[c][2] = centre.z, f.shadowSphere[c][3] = R;
            if (c == 0) f.shadowBias[0] = 1.5f / fit.depthRange;
            const SDL_GPUViewport vp{float(c * q.resolution), 0.0f, float(q.resolution), float(q.resolution), 0.0f, 1.0f};
            SDL_SetGPUViewport(sp, &vp);
            SVU u{};
            std::memcpy(u.vp, fit.viewProj.data(), sizeof(u.vp));
            const float cr = R * 1.5f + kCasterReach; // rough caster cull: farther items cannot reach the map
            auto inReach = [&](const float* mn, const float* mx) {
                for (int a = 0; a < 3; ++a) {
                    const float lo = cc[a] - cr, hi = cc[a] + cr;
                    if (mx[a] < lo || mn[a] > hi) return false;
                }
                return true;
            };
            for (const std::vector<DrawItem>* list : {&items, static_cast<const std::vector<DrawItem>*>(&shadowOnlyItems)}) // + the player's body
            for (const DrawItem& it : *list) {
                if (it.effects || it.depthMax < 1.0f) continue; // sprites; the view model (first-person arms and gun)
                if (it.model) {
                    if (it.model->hasBounds) {
                        float mn[3], mx[3];
                        game::transformedBoxBounds(it.model->mins, it.model->maxs, it.transform.data(), mn, mx);
                        if (!inReach(mn, mx)) continue;
                    }
                    bool any = false;
                    for (const Batch& b : it.model->batches) any = any || ((b.pass == Pass::Opaque || b.pass == Pass::AlphaTest) && b.fxKind == 0);
                    if (!any) continue;
                    const SDL_GPUBufferBinding vb{it.model->vertices, it.model->vertexOffset};
                    SDL_BindGPUVertexBuffers(sp, 0, &vb, 1);
                    const SDL_GPUBufferBinding ib{it.model->indices, 0};
                    SDL_BindGPUIndexBuffer(sp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    u.model = it.transform;
                    if (c == 0) ++enhStats.casters;
                    for (const Batch& b : it.model->batches) {
                        if ((b.pass != Pass::Opaque && b.pass != Pass::AlphaTest) || b.fxKind != 0 || !b.indexCount) continue;
                        u.p[0] = b.pass == Pass::AlphaTest ? 1.0f : 0.0f;
                        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
                        const SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
                        SDL_BindGPUFragmentSamplers(sp, 0, &tb, 1);
                        enhStats.casterTriangles += b.indexCount / 3;
                        drawIndexed(sp, b.indexCount, 1, b.firstIndex, 0, 0);
                    }
                } else if (it.worldBatches) { // brush movers (doors, gates): the world buffers at the entity transform
                    const SDL_GPUBufferBinding vb{scene.vertices, 0};
                    SDL_BindGPUVertexBuffers(sp, 0, &vb, 1);
                    const SDL_GPUBufferBinding ib{scene.indices, 0};
                    SDL_BindGPUIndexBuffer(sp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                    u.model = it.transform;
                    if (c == 0) ++enhStats.casters;
                    for (const Batch& b : *it.worldBatches) {
                        if ((b.pass != Pass::Opaque && b.pass != Pass::AlphaTest) || !b.indexCount) continue;
                        u.p[0] = b.pass == Pass::AlphaTest ? 1.0f : 0.0f;
                        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
                        const SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
                        SDL_BindGPUFragmentSamplers(sp, 0, &tb, 1);
                        enhStats.casterTriangles += b.indexCount / 3;
                        drawIndexed(sp, b.indexCount, 1, b.firstIndex, 0, 0);
                    }
                }
            }
        }
        SDL_EndGPURenderPass(sp);
        shadowCascadesNow = q.cascades;
        f.shadowParams[0] = float(q.cascades);
        f.shadowParams[1] = 1.0f / float(q.resolution);
        f.shadowParams[2] = 1.5f; // normal offset, texels
        f.shadowParams[3] = float(q.pcfTaps);
        f.shadowBias[1] = 1.0f;
    }
    // Light-grid lattices of the model items near the camera (docs/render-enhanced.md "Per-pixel light grid").
    itemLattice.assign(items.size(), EnhDrawUC{});
    latticeData.clear();
    if (enh.gridPerPixel && scene.grid) {
        if (gridCacheOf != scene.serial * 1024 + scene.gridEpoch) gridCache.clear(), gridCacheOf = scene.serial * 1024 + scene.gridEpoch; // samples at grid nodes, kept per map
        constexpr float kLatticeDist = 2500.0f;
        for (size_t i = 0; i < items.size(); ++i) {
            const DrawItem& it = items[i];
            if (!it.model || !it.hasProbe || !it.model->hasBounds || it.effects) continue;
            float mn[3], mx[3];
            game::transformedBoxBounds(it.model->mins, it.model->maxs, it.transform.data(), mn, mx);
            const float dx = 0.5f * (mn[0] + mx[0]) - cam.pos.x, dy = 0.5f * (mn[1] + mx[1]) - cam.pos.y, dz = 0.5f * (mn[2] + mx[2]) - cam.pos.z;
            if (dx * dx + dy * dy + dz * dz > kLatticeDist * kLatticeDist) continue;
            const game::ProbeLattice lat = game::probeLattice(mn, mx);
            const size_t base = latticeData.size() / 28;
            for (int z = 0; z < lat.n[2]; ++z)
                for (int y = 0; y < lat.n[1]; ++y)
                    for (int x = 0; x < lat.n[0]; ++x) {
                        const int64_t gx = lat.node0[0] + x * lat.stride[0], gy = lat.node0[1] + y * lat.stride[1], gz = lat.node0[2] + z * lat.stride[2];
                        const int64_t key = ((gx + 0x100000) << 42) | ((gy + 0x100000) << 21) | (gz + 0x100000);
                        auto hit = gridCache.find(key);
                        if (hit == gridCache.end()) {
                            std::array<float, 28> e{};
                            assets::LightGridCoeffs c;
                            float sunVis = 1.0f;
                            const std::array<float, 3> p = {float(gx) * game::kGridCell[0], float(gy) * game::kGridCell[1], float(gz) * game::kGridCell[2]};
                            if (scene.grid->sample(p, c, sunVis)) {
                                for (int ch = 0; ch < 3; ++ch)
                                    for (int h = 0; h < 2; ++h)
                                        for (int k = 0; k < 4; ++k) e[size_t(ch * 8 + h * 4 + k)] = c[size_t(ch)][size_t(h * 4 + k)];
                                e[24] = sunVis;
                            } else e[27] = -1.0f; // no grid point here: the item's own sample stands in
                            hit = gridCache.emplace(key, e).first;
                        }
                        if (hit->second[27] < 0.0f) {
                            for (int r = 0; r < 6; ++r)
                                for (int k = 0; k < 4; ++k) latticeData.push_back(it.probe[r][k]);
                            latticeData.insert(latticeData.end(), {it.sunVisibility, 0.0f, 0.0f, 0.0f});
                        } else latticeData.insert(latticeData.end(), hit->second.begin(), hit->second.end());
                    }
            EnhDrawUC& d = itemLattice[i];
            d.lattice[0] = float(base), d.lattice[1] = float(lat.n[0]), d.lattice[2] = float(lat.n[1]), d.lattice[3] = float(lat.n[2]);
            for (int a = 0; a < 3; ++a) d.origin[a] = lat.origin[a], d.step[a] = lat.step[a];
            ++enhStats.latticeItems;
            enhStats.latticePoints += lat.count();
        }
        if (!latticeData.empty()) {
            const uint32_t bytes = uint32_t(latticeData.size() * sizeof(float));
            if (bytes > latticeCap) {
                SDL_ReleaseGPUBuffer(dev, latticeBuf);
                SDL_ReleaseGPUTransferBuffer(dev, latticeXfer);
                latticeCap = std::max(bytes, latticeCap * 2);
                SDL_GPUBufferCreateInfo bi{};
                bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
                bi.size = latticeCap;
                latticeBuf = SDL_CreateGPUBuffer(dev, &bi);
                SDL_GPUTransferBufferCreateInfo ti{};
                ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                ti.size = latticeCap;
                latticeXfer = SDL_CreateGPUTransferBuffer(dev, &ti);
                if (!latticeBuf || !latticeXfer) sdlFail("lattice buffer");
            }
            void* dst = SDL_MapGPUTransferBuffer(dev, latticeXfer, true);
            std::memcpy(dst, latticeData.data(), bytes);
            SDL_UnmapGPUTransferBuffer(dev, latticeXfer);
            SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTransferBufferLocation src{latticeXfer, 0};
            const SDL_GPUBufferRegion region{latticeBuf, 0, bytes};
            SDL_UploadToGPUBuffer(cp, &src, &region, true);
            SDL_EndGPUCopyPass(cp);
        }
    }
    lightShadowsNow = 0;
    // b4: the two strongest map lights near the camera that reach a moving model get its shadows (docs/render-enhanced.md)
    bakedShadowLights.clear();
    if (enh.lightShadows && !scene.mapLights.empty()) {
        std::vector<std::pair<float, const game::MapLight*>> cand;
        for (const game::MapLight& l : scene.mapLights) {
            const float camPos[3] = {cam.pos.x, cam.pos.y, cam.pos.z};
            if (!game::bakedShadowLightNearCamera(l, camPos)) continue;
            const float dx = l.pos[0] - cam.pos.x, dy = l.pos[1] - cam.pos.y, dz = l.pos[2] - cam.pos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            bool reaches = false;
            for (const DrawItem& it : items) {
                if (!it.model || it.effects || it.depthMax < 1.0f || !it.model->hasBounds) continue;
                float mn[3], mx[3];
                game::transformedBoxBounds(it.model->mins, it.model->maxs, it.transform.data(), mn, mx);
                if (game::lightTouchesBox(l.pos, l.radius, mn, mx)) { reaches = true; break; }
            }
            if (!reaches) continue;
            const float power = (l.color[0] + l.color[1] + l.color[2]) * l.radius / std::max(d, 50.0f);
            cand.push_back({-power, &l});
        }
        std::sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t k = 0; k < cand.size() && k < 2; ++k) {
            const game::MapLight& l = *cand[k].second;
            bakedShadowLights.push_back(&l);
            for (int a = 0; a < 3; ++a) f.bsPos[k][a] = l.pos[a], f.bsColor[k][a] = l.color[a];
            f.bsPos[k][3] = l.radius, f.bsColor[k][3] = float(kLightShadowMax + int(k));
        }
        f.bsParams[0] = float(bakedShadowLights.size()), f.bsParams[1] = lightShadowNear;
        if (rtShadowNow && enh.rtShadows) f.bsParams[2] = 1.0f; // stage 5: the lamps' visibility comes from the AO image's alpha
    }
    if (enh.lightShadows && (!dlights.empty() || !bakedShadowLights.empty())) renderLightShadows(cmd, scene, items);
    if (!lightShadowAtlas && f.bsParams[2] < 0.5f) f.bsParams[0] = 0.0f;
    // Materials: GGX, reflections (the sky cube with mips; screen-space from the previous frame while TAA has a history).
    ensureEnvCube(cmd, scene);
    {
        const Mat4 vp = cam.viewProj(frameAspect());
        std::memcpy(f.viewProj, vp.data(), sizeof f.viewProj);
        std::memcpy(f.prevViewProj, (taaPrevVPValid ? taaPrevVP : vp).data(), sizeof f.prevViewProj);
        game::Mat4c cur{};
        std::memcpy(cur.data(), vp.data(), sizeof(float) * 16);
        const Vec3 fw = cam.forward();
        const float eye[3] = {cam.pos.x, cam.pos.y, cam.pos.z}, fwd[3] = {fw.x, fw.y, fw.z};
        const auto zl = game::clipDepthLine(cur, eye, fwd);
        const bool ssrOn = enh.ggx && enh.ssr && enh.taa && taaHistValid && taaPrevVPValid && taaHist[taaCur] && taaW == outW && taaH == outH;
        f.ssrParams[0] = zl[0], f.ssrParams[1] = zl[1], f.ssrParams[2] = ssrOn ? 1.0f : 0.0f, f.ssrParams[3] = 262144.0f * 2.0f * 1.7f;
        frameHist = ssrOn ? taaHist[taaCur] : nullptr;
        frameDepth = nullptr;
        f.specParams[0] = enh.ggx ? 1.0f : 0.0f, f.specParams[1] = enh.specScale, f.specParams[2] = enh.envScale, f.specParams[3] = float(std::max(envLevels - 1, 0));
        f.mipBias[0] = std::log2(renderScaleNow); // textures at the output size's detail while the scene renders smaller
        if (rtReflNow) f.rtParams[0] = 1.0f, f.rtParams[1] = game::kRtReflectionMaxAlpha, f.rtParams[2] = game::kRtReflectionRange;
        static const bool rtDebug = std::getenv("IW2_RT_DEBUG") != nullptr; // debug: reflections red (on-screen hit), green (other hit), blue (miss)
        if (rtReflNow && rtDebug) f.rtParams[3] = 1.0f;
        if (enh.probes && probeCount > 0 && probeIdxBuf && !capturingProbes) {
            for (int a = 0; a < 3; ++a) f.probeGrid[0][a] = probeGrid.origin[a], f.probeGrid[1][a] = float(probeGrid.dims[a]);
            f.probeGrid[0][3] = 1.0f / probeGrid.cell, f.probeGrid[1][3] = 1.0f;
        }
    }
    if (enh.ggx && enh.mapLightSpec && !scene.mapLights.empty()) { // the 8 map lights nearest the camera that reach within 1500 units
        std::vector<std::pair<float, size_t>> near;
        for (size_t i = 0; i < scene.mapLights.size(); ++i) {
            const game::MapLight& l = scene.mapLights[i];
            const float dx = l.pos[0] - cam.pos.x, dy = l.pos[1] - cam.pos.y, dz = l.pos[2] - cam.pos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - l.radius;
            if (d < 1500.0f) near.push_back({d, i});
        }
        std::sort(near.begin(), near.end());
        const size_t n = std::min<size_t>(near.size(), 8);
        for (size_t k = 0; k < n; ++k) {
            const game::MapLight& l = scene.mapLights[near[k].second];
            for (int a = 0; a < 3; ++a) f.mlPos[k][a] = l.pos[a], f.mlColor[k][a] = l.color[a], f.mlDir[k][a] = l.dir[a];
            f.mlPos[k][3] = l.radius, f.mlColor[k][3] = l.cosInner, f.mlDir[k][3] = l.cosOuter;
        }
        f.mlParams[0] = float(n);
    }
    aoReady = false;
    if (enh.ao || enh.contactShadows || rtShadowNow) {
        f.aoParams[2] = enh.ao ? enh.aoStrength : 0.0f;
        f.aoParams[3] = enh.contactShadows || rtShadowNow ? 1.0f : 0.0f; // the contact channel also carries the ray-traced sun shadow
    }
    waterSunNow = game::waterSunShadowSource(sunLit, rtShadowNow && enh.rtShadows, shadowCascadesNow);
    f.waterParams[0] = float(int(waterSunNow)), f.waterParams[1] = float(frameCounter % 4096u), f.waterParams[2] = game::kWaterShadowDarken;
    enhFragPending = f;
    SDL_PushGPUFragmentUniformData(cmd, 1, &f, sizeof f);
}

// One cached world cascade (docs/render-enhanced.md "World sun shadows"): the static world and the static models (LOD 0) whose box
// reaches the cascade's square in the light plane, opaque and alpha-tested surfaces, into its own depth texture.
void Renderer::renderWorldCascade(SDL_GPUCommandBuffer* cmd, const Scene& scene, int c, const game::WorldShadowQuality& q, const float sun[3]) {
    WorldCascade& wc = worldCascades[c];
    const float centre[3] = {wc.centre[0], wc.centre[1], wc.centre[2]};
    const float R = q.radius[c];
    const float reach = game::casterReachForBounds(sun, centre, worldMins, worldMaxs, R);
    const game::SunShadowFit fit = game::fitSunShadow(sun, centre, R, reach, q.resolution);
    std::memcpy(wc.vp, fit.viewProj.data(), sizeof wc.vp);
    wc.depthBias = 2.0f * fit.texelWorld / fit.depthRange;
    // Caster ranges: pieces whose box projects into the cascade square.
    auto boxIn = [&](const float* mn, const float* mx) {
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < 8; ++k) {
            const float p[3] = {(k & 1) ? mx[0] : mn[0], (k & 2) ? mx[1] : mn[1], (k & 4) ? mx[2] : mn[2]};
            const auto cl = game::mulPoint(fit.viewProj, p);
            for (int a = 0; a < 2; ++a) lo[a] = std::min(lo[a], cl[size_t(a)]), hi[a] = std::max(hi[a], cl[size_t(a)]);
        }
        return hi[0] >= -1.0f && lo[0] <= 1.0f && hi[1] >= -1.0f && lo[1] <= 1.0f;
    };
    std::vector<uint32_t> idx;
    wc.batches.clear();
    for (const Batch& src : scene.batches) {
        if ((src.pass != Pass::Opaque && src.pass != Pass::AlphaTest) || src.fxKind != 0) continue;
        Batch b = src;
        b.firstIndex = uint32_t(idx.size());
        for (uint32_t k = 0; k < src.pieceCount; ++k) {
            const CullPiece& pc = scene.cullPieces[src.pieceFirst + k];
            const float *mn, *mx;
            if (pc.owner >= 0) {
                if (pc.lod != 0) continue;
                mn = scene.staticModels[size_t(pc.owner)].mins, mx = scene.staticModels[size_t(pc.owner)].maxs;
            } else {
                mn = &scene.soupBoxes[size_t(-pc.owner - 1)][0], mx = &scene.soupBoxes[size_t(-pc.owner - 1)][3];
            }
            if (!boxIn(mn, mx)) continue;
            idx.insert(idx.end(), scene.cpuIndices.begin() + pc.firstIndex, scene.cpuIndices.begin() + pc.firstIndex + pc.indexCount);
        }
        b.indexCount = uint32_t(idx.size()) - b.firstIndex;
        if (b.indexCount) wc.batches.push_back(b);
    }
    const uint32_t bytes = uint32_t(idx.size() * 4);
    if (bytes > wc.indexCap) {
        if (wc.indices) SDL_ReleaseGPUBuffer(dev, wc.indices);
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_INDEX;
        bi.size = wc.indexCap = std::max(bytes, wc.indexCap * 2);
        wc.indices = SDL_CreateGPUBuffer(dev, &bi);
        if (!wc.indices) sdlFail("cascade index buffer");
    }
    if (bytes) {
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = bytes;
        SDL_GPUTransferBuffer* xfer = SDL_CreateGPUTransferBuffer(dev, &ti);
        void* dst = SDL_MapGPUTransferBuffer(dev, xfer, false);
        std::memcpy(dst, idx.data(), bytes);
        SDL_UnmapGPUTransferBuffer(dev, xfer);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUTransferBufferLocation src{xfer, 0};
        const SDL_GPUBufferRegion region{wc.indices, 0, bytes};
        SDL_UploadToGPUBuffer(cp, &src, &region, false);
        SDL_EndGPUCopyPass(cp);
        SDL_ReleaseGPUTransferBuffer(dev, xfer);
    }
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = worldShadowArray;
    dt.layer = uint8_t(c);
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* sp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt);
    SDL_BindGPUGraphicsPipeline(sp, enhShadowPipe);
    if (bytes) {
        const SDL_GPUBufferBinding vb{scene.vertices, 0};
        SDL_BindGPUVertexBuffers(sp, 0, &vb, 1);
        const SDL_GPUBufferBinding ib{wc.indices, 0};
        SDL_BindGPUIndexBuffer(sp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        struct SVU { float vp[16]; Mat4 model; float p[4]; } u{};
        std::memcpy(u.vp, fit.viewProj.data(), sizeof u.vp);
        u.model = kIdentity;
        for (const Batch& b : wc.batches) {
            u.p[0] = b.pass == Pass::AlphaTest ? 1.0f : 0.0f;
            SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
            const SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
            SDL_BindGPUFragmentSamplers(sp, 0, &tb, 1);
            drawIndexed(sp, b.indexCount, 1, b.firstIndex, 0, 0);
        }
    }
    SDL_EndGPURenderPass(sp);
    ++enhStats.cascadeRenders;
}

// TAA (docs/render-enhanced.md "Temporal anti-aliasing"): the $floatz pass with the motion vectors as a second target. Each item's
// previous transform and, for skinned models, its previous skinned vertices (a copy of last frame's skin arena) give the previous
// clip position per vertex; the world's previous position is its own (camera motion only).
void Renderer::enhFloatzVelocity(SDL_GPUCommandBuffer* cmd, const SDL_GPUColorTargetInfo& fz, const SDL_GPUDepthStencilTargetInfo& fds, const Scene& scene,
                                 const Camera& cam, const Camera& camIn, const std::vector<DrawItem>& items, uint32_t w, uint32_t h) {
    ensureVelocity(w, h);
    SDL_GPUColorTargetInfo ct[2] = {fz, {}};
    ct[1].texture = velocityTex;
    ct[1].clear_color = {0, 0, 0, 0};
    ct[1].load_op = SDL_GPU_LOADOP_CLEAR;
    ct[1].store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* fp = SDL_BeginGPURenderPass(cmd, ct, 2, &fds);
    stats.phase = 1;
    const float aspect = frameAspect();
    struct VVU { Mat4 vp; float eye[4]; Mat4 model; Mat4 curVP; Mat4 prevVP; Mat4 prevModel; };
    const Mat4 vpJ = cam.viewProj(aspect), vpU = camIn.viewProj(aspect);
    Mat4 prevVP = taaPrevVPValid ? taaPrevVP : vpU;
    if (const char* py = std::getenv("IW2_ENH_PREV_CAM")) prevVP = debugPrevCamViewProj(camIn, py, aspect); // debug (--shot)
    float viewModelMask = 0.0f; // 1 while the view model is drawn: the velocity image's blue channel (motion blur leaves it out)
    auto drawV = [&](const Batch& b, VVU u) {
        if (b.pass != Pass::Opaque && b.pass != Pass::AlphaTest) return;
        SDL_BindGPUGraphicsPipeline(fp, enhFloatzVelPipes[std::clamp(int(b.polygonOffset), 0, 2)][std::clamp(b.cull, 0, 2)]);
        u.eye[3] = float(b.polygonOffset) * kPolygonOffsetBias / 65536.0f;
        SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
        const SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
        SDL_BindGPUFragmentSamplers(fp, 0, &tb, 1);
        const float fzu[4] = {b.pass == Pass::AlphaTest ? 1.0f : 0.0f, viewModelMask, 0, 0};
        SDL_PushGPUFragmentUniformData(cmd, 0, fzu, sizeof(fzu));
        drawIndexed(fp, b.indexCount, 1, b.firstIndex, 0, 0);
    };
    const VVU base{vpJ, {cam.pos.x, cam.pos.y, cam.pos.z, 0}, kIdentity, vpU, prevVP, kIdentity};
    const SDL_GPUBufferBinding wvb[2] = {{scene.vertices, 0}, {scene.vertices, 0}};
    SDL_BindGPUVertexBuffers(fp, 0, wvb, 2);
    if (culledIndexBuf) {
        const SDL_GPUBufferBinding cib{culledIndexBuf, 0};
        SDL_BindGPUIndexBuffer(fp, &cib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (const Batch& b : culledBatches)
            if (b.indexCount) drawV(b, base);
    }
    // stable keys for the items: the model (or brush batches) and its occurrence in this frame's list
    itemKeys.assign(items.size(), 0);
    {
        std::unordered_map<const void*, uint32_t> occ;
        for (size_t i = 0; i < items.size(); ++i) {
            const void* id = items[i].model ? static_cast<const void*>(items[i].model) : static_cast<const void*>(items[i].worldBatches);
            const uint32_t n = occ[id]++;
            itemKeys[i] = uint64_t(reinterpret_cast<uintptr_t>(id)) * 0x9E3779B97F4A7C15ull ^ (uint64_t(n) * 0xC2B2AE3D27D4EB4Full);
        }
    }
    curItems.clear();
    const SDL_GPUBufferBinding fib{scene.indices, 0};
    SDL_BindGPUIndexBuffer(fp, &fib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    for (size_t i = 0; i < items.size(); ++i) {
        const DrawItem& it = items[i];
        if (!it.worldBatches) continue;
        VVU u = base;
        u.model = it.transform;
        const auto pv = prevItems.find(itemKeys[i]);
        u.prevModel = pv != prevItems.end() ? pv->second.transform : it.transform;
        curItems[itemKeys[i]] = {it.transform, 0, 0, false};
        for (const Batch& b : *it.worldBatches) drawV(b, u);
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const DrawItem& it = items[i];
        if (!it.model) continue;
        const SDL_GPUViewport fzRange{0, 0, float(w), float(h), 0.0f, it.depthMax};
        SDL_SetGPUViewport(fp, &fzRange);
        const bool arena = it.model->arenaVertices && it.model->vertices == scene.skinArena;
        PrevItem cur{it.transform, arena ? it.model->vertexOffset : 0, arena ? it.model->arenaBytes : 0, arena};
        curItems[itemKeys[i]] = cur;
        const auto pv = prevItems.find(itemKeys[i]);
        SDL_GPUBufferBinding vb[2] = {{it.model->vertices, it.model->vertexOffset}, {it.model->vertices, it.model->vertexOffset}};
        VVU u = base;
        u.model = it.transform;
        u.prevModel = it.transform;
        if (it.depthMax < 1.0f) { // the view model moves with the camera: no motion
            u.vp = cam.viewProj(aspect, it.zNear, true);
            u.curVP = u.prevVP = camIn.viewProj(aspect, it.zNear, true);
        } else if (pv != prevItems.end()) {
            u.prevModel = pv->second.transform;
            if (arena && pv->second.arena && pv->second.arenaBytes == cur.arenaBytes && prevArenaValid && prevArena &&
                pv->second.arenaOffset + cur.arenaBytes <= prevArenaCap)
                vb[1] = {prevArena, pv->second.arenaOffset};
        }
        SDL_BindGPUVertexBuffers(fp, 0, vb, 2);
        const SDL_GPUBufferBinding mib{it.model->indices, 0};
        SDL_BindGPUIndexBuffer(fp, &mib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        viewModelMask = it.depthMax < 1.0f ? 1.0f : 0.0f;
        for (const Batch& b : it.model->batches) drawV(b, u);
    }
    SDL_EndGPURenderPass(fp);
    stats.phase = 0;
    velocityFromFloatz = true;
}

// The history resolve into the scene image (before exposure, bloom and the tone curve).
void Renderer::enhTaaResolve(SDL_GPUCommandBuffer* cmd, uint32_t rw, uint32_t rh, uint32_t w, uint32_t h, const Scene& scene, const Camera& cam) {
    if (!taaHist[0] || taaW != w || taaH != h) {
        for (SDL_GPUTexture*& t : taaHist) {
            if (t) SDL_ReleaseGPUTexture(dev, t);
            SDL_GPUTextureCreateInfo ti{};
            ti.type = SDL_GPU_TEXTURETYPE_2D;
            ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            if (metalCaps.metalFx) ti.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE; // the MetalFX output
            ti.width = w, ti.height = h, ti.layer_count_or_depth = 1, ti.num_levels = 1;
            t = SDL_CreateGPUTexture(dev, &ti);
            if (!t) sdlFail("TAA history");
        }
        taaW = w, taaH = h;
        taaHistValid = false;
    }
    if (taaScene != scene.serial) taaHistValid = false, taaScene = scene.serial;
    const Mat4 vp = cam.viewProj(float(w) / float(h));
    game::Mat4c cur{}, inv{};
    std::memcpy(cur.data(), vp.data(), sizeof(float) * 16);
    if (!game::invertMat(cur, inv)) return;
    const Vec3 fw = cam.forward();
    const float eye[3] = {cam.pos.x, cam.pos.y, cam.pos.z}, fwd[3] = {fw.x, fw.y, fw.z};
    const auto zl = game::clipDepthLine(cur, eye, fwd);
    struct { float inv[16], prev[16], z[4], size[4], react[4]; } u{};
    std::memcpy(u.inv, inv.data(), sizeof u.inv);
    std::memcpy(u.prev, (taaPrevVPValid ? taaPrevVP : vp).data(), sizeof u.prev);
    u.z[0] = zl[0], u.z[1] = zl[1], u.z[2] = 262144.0f * 2.0f * 1.7f, u.z[3] = taaHistValid && taaPrevVPValid ? 1.0f : 0.0f;
    u.size[0] = 1.0f / float(rw), u.size[1] = 1.0f / float(rh), u.size[2] = cam.jitterX, u.size[3] = cam.jitterY;
    const bool shadowReact = aoReady && aoPrevValid; // the moving objects' shadow change (docs/render-enhanced.md "Moving shadows in the temporal filter")
    u.react[0] = shadowReact ? 1.0f : 0.0f;
    u.react[1] = game::kTaaShadowChangeStart, u.react[2] = game::kTaaShadowChangeFull, u.react[3] = game::kTaaShadowHistoryWeight;
    enhNanInject(cmd, sceneTex, rw, rh, false); // IW2_ENH_INJECT_NAN: into the resolve's input
    if (const char* nc = std::getenv("IW2_ENH_NANCHECK"); nc && std::atoi(nc) == 2) { // debug: count the input instead (report only)
        nanInputMode = true;
        enhNanGuard(cmd, sceneTex, rw, rh);
    }
    if (enh.metalFx && metalCaps.metalFx && enhMetalFxResolve(cmd, rw, rh, w, h, cam, &u, sizeof u)) {
        enhNanGuard(cmd, taaHist[taaCur], w, h);
        return;
    }
    if (scaledActive) { // MetalFX failed with a render scale: stretch the scene to the output size, no anti-aliasing this frame
        SDL_GPUBlitInfo bi{};
        bi.source.texture = sceneTex, bi.source.w = rw, bi.source.h = rh;
        restoreScaledScene();
        bi.destination.texture = sceneTex, bi.destination.w = w, bi.destination.h = h;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE, bi.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &bi);
        taaHistValid = false;
        return;
    }
    SDL_GPUColorTargetInfo ct{};
    ct.texture = taaHist[1 - taaCur];
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhTaaPipe);
    const SDL_GPUTextureSamplerBinding tb[6] = {{sceneTex, pointClampSampler}, {taaHist[taaCur], clampSampler}, {velocityTex, pointClampSampler},
                                                {floatzTex, pointClampSampler}, {shadowReact ? aoTex[0] : aoWhite, clampSampler},
                                                {shadowReact ? aoPrevTex : aoWhite, clampSampler}};
    SDL_BindGPUFragmentSamplers(p, 0, tb, 6);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd); // the resolved frame replaces the scene image for the rest of the frame
    SDL_GPUTextureLocation src{}, dst{};
    src.texture = taaHist[1 - taaCur];
    dst.texture = sceneTex;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, w, h, 1, false);
    SDL_EndGPUCopyPass(cp);
    taaCur = 1 - taaCur;
    taaHistValid = true;
    enhNanGuard(cmd, taaHist[taaCur], w, h);
}

// IW2_ENH_INJECT_NAN=x,y,w,h,frames[,history] (docs/debug-flags.md): NaN into that rectangle of the resolve's input (or, with
// history = 1, of the history after the resolve) for the first `frames` TAA frames: the test of the sanitising and the guard.
void Renderer::enhNanInject(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* img, uint32_t w, uint32_t h, bool history) {
    static const char* spec = std::getenv("IW2_ENH_INJECT_NAN");
    if (!spec || !enhNanInjectPipe || !img) return;
    int r[6] = {0, 0, 0, 0, 0, 0};
    std::sscanf(spec, "%d,%d,%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3], &r[4], &r[5]);
    if ((r[5] != 0) != history || taaFrame >= uint32_t(std::max(r[4], 0)) || r[2] <= 0 || r[3] <= 0) return;
    SDL_GPUColorTargetInfo ct{};
    ct.texture = img, ct.load_op = SDL_GPU_LOADOP_LOAD, ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhNanInjectPipe);
    const SDL_Rect sc{std::min(r[0], int(w) - 1), std::min(r[1], int(h) - 1), r[2], r[3]};
    SDL_SetGPUScissor(p, &sc);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    std::cout << "nan inject: " << (history ? "history" : "input") << " frame " << taaFrame << "\n";
}

// The non-finite guard (docs/render-enhanced.md "Non-finite values"): the count of kNanLag frames ago is read without waiting (it
// is long done); a count above 0 drops the TAA / MetalFX history, which a non-finite pixel would otherwise spread. Then this
// frame's count is made on the GPU and its download queued.
void Renderer::enhNanGuard(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* img, uint32_t w, uint32_t h) {
    if (!enhNanCountPipe || !img || (nanInputMode && img != sceneTex)) return;
    enhNanInject(cmd, img, w, h, true); // IW2_ENH_INJECT_NAN with history = 1
    static const bool report = std::getenv("IW2_ENH_NANCHECK") != nullptr;
    if (nanSeq >= kNanLag && nanTb[(nanSeq - kNanLag) % kNanSlots]) {
        const float n = enhNanSlot((nanSeq - kNanLag) % kNanSlots);
        nanLastCount = n;
        if (nanInputMode) {
            if (n > 0.0f) std::cout << "nan input: " << n << " non-finite pixels before the resolve\n";
        } else if (game::nanGuardResets(n)) {
            taaHistValid = false; // our TAA and MetalFX (reset) start afresh
            ++nanResets;
            std::cout << "nan guard: " << n << " non-finite pixels, history reset (" << nanResets << ")\n";
        } else if (report && nanSeq % 60 == 0) {
            std::cout << "nan check: 0\n";
        }
    }
    if (!nanTex) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D, ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = ti.height = kNanGrid, ti.layer_count_or_depth = 1, ti.num_levels = 1;
        nanTex = SDL_CreateGPUTexture(dev, &ti);
        SDL_GPUTransferBufferCreateInfo bi{};
        bi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bi.size = kNanGrid * kNanGrid * 4;
        for (auto& t : nanTb) t = SDL_CreateGPUTransferBuffer(dev, &bi);
        if (!nanTex) return;
    }
    SDL_GPUColorTargetInfo ct{};
    ct.texture = nanTex, ct.load_op = SDL_GPU_LOADOP_DONT_CARE, ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhNanCountPipe);
    const SDL_GPUTextureSamplerBinding tb{img, pointClampSampler};
    SDL_BindGPUFragmentSamplers(p, 0, &tb, 1);
    const float nu[4] = {float(w), float(h), float((w + kNanGrid - 1) / kNanGrid), float((h + kNanGrid - 1) / kNanGrid)};
    SDL_PushGPUFragmentUniformData(cmd, 0, nu, sizeof nu);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region{};
    region.texture = nanTex, region.w = kNanGrid, region.h = kNanGrid, region.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = nanTb[nanSeq % kNanSlots];
    SDL_DownloadFromGPUTexture(cp, &region, &dst);
    SDL_EndGPUCopyPass(cp);
    ++nanSeq;
}

float Renderer::enhNanSlot(uint32_t slot) {
    if (!nanTb[slot]) return 0.0f;
    const auto* v = static_cast<const float*>(SDL_MapGPUTransferBuffer(dev, nanTb[slot], false));
    if (!v) return 0.0f;
    std::vector<float> cells(v, v + kNanGrid * kNanGrid);
    SDL_UnmapGPUTransferBuffer(dev, nanTb[slot]);
    return game::nanGuardSum(cells.data(), cells.size());
}

float Renderer::enhNanCountNow() { return nanSeq > 0 ? enhNanSlot(uint32_t((nanSeq - 1) % kNanSlots)) : 0.0f; }

// MetalFX temporal scaling (docs/render-enhanced.md "Stage 5"): the motion of every pixel and a device depth from $floatz, then
// the scaler on SDL's command buffer (no SDL pass open) into the history texture, which replaces the scene image as the TAA's does.
bool Renderer::enhMetalFxResolve(SDL_GPUCommandBuffer* cmd, uint32_t rw, uint32_t rh, uint32_t w, uint32_t h, const Camera& cam,
                                 const void* u, size_t usize) {
    if (!mfxMotion || mfxW != rw || mfxH != rh) {
        for (SDL_GPUTexture** t : {&mfxMotion, &mfxDepth, &mfxReactive}) if (*t) SDL_ReleaseGPUTexture(dev, *t), *t = nullptr;
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = rw, ti.height = rh, ti.layer_count_or_depth = 1, ti.num_levels = 1;
        ti.format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
        mfxMotion = SDL_CreateGPUTexture(dev, &ti);
        ti.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        mfxDepth = SDL_CreateGPUTexture(dev, &ti);
        ti.format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
        mfxReactive = SDL_CreateGPUTexture(dev, &ti);
        if (!mfxMotion || !mfxDepth) return false;
        mfxW = rw, mfxH = rh;
    }
    auto pass = [&](SDL_GPUGraphicsPipeline* pipe, SDL_GPUTexture* dst, const SDL_GPUTextureSamplerBinding* tb, uint32_t n) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = dst;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(p, pipe);
        SDL_BindGPUFragmentSamplers(p, 0, tb, n);
        SDL_PushGPUFragmentUniformData(cmd, 0, u, uint32_t(usize));
        SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
        SDL_EndGPURenderPass(p);
    };
    const SDL_GPUTextureSamplerBinding mb[2] = {{velocityTex, pointClampSampler}, {floatzTex, pointClampSampler}};
    pass(enhMfxMotionPipe, mfxMotion, mb, 2);
    const SDL_GPUTextureSamplerBinding db[1] = {{floatzTex, pointClampSampler}};
    pass(enhMfxDepthPipe, mfxDepth, db, 1);
    // the reactive mask: where a moving object's shadow came or went, MetalFX takes the current frame (enh_shadow_change)
    if (mfxReactive && enhMfxReactivePipe) {
        const SDL_GPUTextureSamplerBinding rb[4] = {{velocityTex, pointClampSampler}, {floatzTex, pointClampSampler},
                                                    {aoReady ? aoTex[0] : aoWhite, clampSampler}, {aoPrevValid ? aoPrevTex : aoWhite, clampSampler}};
        pass(enhMfxReactivePipe, mfxReactive, rb, 4);
    }
    if (temporalHold && taaHistValid && taaW == w && taaH == h) { // paused: the last output again, no scaler run
        restoreScaledScene();
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src{}, dst{};
        src.texture = taaHist[taaCur];
        dst.texture = sceneTex;
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, w, h, 1, false);
        SDL_EndGPUCopyPass(cp);
        if (!temporalHeld) std::cout << "MetalFX: paused, the last frame is kept\n";
        temporalHeld = true;
        return true;
    }
    if (temporalHeld) std::cout << "MetalFX: running again, history reset\n";
    const bool reset = !(taaHistValid && taaPrevVPValid) || std::exchange(temporalHeld, false);
    // the colour without NaN / infinities (fs_enh_finite_copy): one such value in the scaler's history spreads (black blocks)
    if (!mfxColor || mfxColorW != rw || mfxColorH != rh) {
        if (mfxColor) SDL_ReleaseGPUTexture(dev, mfxColor);
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D, ci.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ci.width = rw, ci.height = rh, ci.layer_count_or_depth = 1, ci.num_levels = 1;
        mfxColor = SDL_CreateGPUTexture(dev, &ci);
        mfxColorW = rw, mfxColorH = rh;
    }
    SDL_GPUTexture* mfxIn = sceneTex;
    if (mfxColor && enhFiniteCopyPipe) {
        const SDL_GPUTextureSamplerBinding cb[1] = {{sceneTex, pointClampSampler}};
        pass(enhFiniteCopyPipe, mfxColor, cb, 1);
        mfxIn = mfxColor;
    }
    if (!metalFx.encode(dev, cmd, mfxIn, mfxDepth, mfxMotion, mfxReactive, taaHist[1 - taaCur], rw, rh, w, h, cam.jitterX, cam.jitterY, reset)) {
        if (!metalFxLogged) metalFxLogged = true, std::cout << metalFx.describe() << " (our TAA instead)\n";
        return false;
    }
    if (!metalFxLogged) metalFxLogged = true, std::cout << metalFx.describe() << "\n";
    restoreScaledScene(); // from here on the frame is at the output size
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation src{}, dst{};
    src.texture = taaHist[1 - taaCur];
    dst.texture = sceneTex;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, w, h, 1, false);
    SDL_EndGPUCopyPass(cp);
    taaCur = 1 - taaCur;
    taaHistValid = true;
    return true;
}

// End of a TAA frame: keep this frame's skinned vertices, item transforms and camera for the next frame's motion vectors.
void Renderer::enhTaaEndFrame(SDL_GPUCommandBuffer* cmd, const Scene& scene, const Camera& camIn, uint32_t w, uint32_t h, const std::vector<DrawItem>& items) {
    (void)items;
    uint32_t used = 0;
    for (const auto& [k, v] : curItems)
        if (v.arena) used = std::max(used, v.arenaOffset + v.arenaBytes);
    prevArenaValid = false;
    if (used > 0 && scene.skinArena && used <= scene.skinArenaSize) {
        if (used > prevArenaCap) {
            if (prevArena) SDL_ReleaseGPUBuffer(dev, prevArena);
            SDL_GPUBufferCreateInfo bi{};
            bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
            bi.size = prevArenaCap = std::max(used + used / 2, 4u << 20);
            prevArena = SDL_CreateGPUBuffer(dev, &bi);
            if (!prevArena) sdlFail("previous skin arena");
        }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        const SDL_GPUBufferLocation src{scene.skinArena, 0}, dst{prevArena, 0};
        SDL_CopyGPUBufferToBuffer(cp, &src, &dst, used, false);
        SDL_EndGPUCopyPass(cp);
        prevArenaValid = true;
    }
    prevItems.swap(curItems);
    taaPrevVP = camIn.viewProj(float(w) / float(h));
    taaPrevVPValid = true;
    ++taaFrame;
}

// GTAO and contact shadows from $floatz at half size, then a depth-aware blur (docs/render-enhanced.md "Ambient occlusion").
void Renderer::enhAO(SDL_GPUCommandBuffer* cmd, uint32_t w, uint32_t h, const Camera& cam, const Scene& scene) {
    const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
    if (!aoTex[0] || aoW != hw || aoH != hh) {
        for (SDL_GPUTexture*& t : aoTex) {
            if (t) SDL_ReleaseGPUTexture(dev, t);
            SDL_GPUTextureCreateInfo ti{};
            ti.type = SDL_GPU_TEXTURETYPE_2D;
            ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ti.width = hw, ti.height = hh, ti.layer_count_or_depth = 1, ti.num_levels = 1;
            t = SDL_CreateGPUTexture(dev, &ti);
            if (!t) sdlFail("AO texture");
        }
        if (aoPrevTex) SDL_ReleaseGPUTexture(dev, aoPrevTex);
        SDL_GPUTextureCreateInfo pi{};
        pi.type = SDL_GPU_TEXTURETYPE_2D, pi.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, pi.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        pi.width = hw, pi.height = hh, pi.layer_count_or_depth = 1, pi.num_levels = 1;
        aoPrevTex = SDL_CreateGPUTexture(dev, &pi);
        aoLastFrame = ~0u;
        aoW = hw, aoH = hh;
    }
    const Mat4 vp = cam.viewProj(frameAspect());
    game::Mat4c cur{}, inv{};
    std::memcpy(cur.data(), vp.data(), sizeof(float) * 16);
    if (!game::invertMat(cur, inv)) return;
    const Vec3 fw = cam.forward();
    const float eye[3] = {cam.pos.x, cam.pos.y, cam.pos.z}, fwd[3] = {fw.x, fw.y, fw.z};
    const auto zl = game::clipDepthLine(cur, eye, fwd);
    // focal length in pixels: the projected size of one unit at depth 1 (vertical)
    const Vec3 upv = cross(cam.right(), fw);
    const float p1[3] = {eye[0] + fw.x * 100.0f, eye[1] + fw.y * 100.0f, eye[2] + fw.z * 100.0f};
    const float p2[3] = {p1[0] + upv.x, p1[1] + upv.y, p1[2] + upv.z};
    const auto c1 = game::mulPoint(cur, p1), c2 = game::mulPoint(cur, p2);
    const float focal = std::fabs(c2[1] / c2[3] - c1[1] / c1[3]) * 0.5f * float(h) * 100.0f;
    struct { float inv[16], vp[16], z[4], eye[4], sun[4], params[4], size[4], rt[4]; } u{};
    std::memcpy(u.inv, inv.data(), sizeof u.inv);
    std::memcpy(u.vp, vp.data(), sizeof u.vp);
    u.z[0] = zl[0], u.z[1] = zl[1], u.z[2] = 262144.0f * 2.0f * 1.7f, u.z[3] = float(aoFrame++ % 64);
    u.eye[0] = eye[0], u.eye[1] = eye[1], u.eye[2] = eye[2], u.eye[3] = focal;
    u.sun[0] = sunDirForAO[0], u.sun[1] = sunDirForAO[1], u.sun[2] = sunDirForAO[2], u.sun[3] = enh.contactShadows ? 1.0f : 0.0f;
    u.params[0] = 40.0f, u.params[1] = enh.ao ? 1.0f : 0.0f, u.params[2] = 18.0f, u.params[3] = 10.0f;
    u.size[0] = 1.0f / float(w), u.size[1] = 1.0f / float(h), u.size[2] = float(w), u.size[3] = float(h);
    // Stage 5: the moving objects' ray-traced sun / lamp shadows and contact occlusion at the AO's half size (docs/render-enhanced.md).
    if (rtShadowNow && frameItems) {
        prepareSceneRT(cmd, scene, *frameItems);
        if (!rtShadowTex || rtShadowW != hw || rtShadowH != hh) {
            for (SDL_GPUTexture** t : {&rtShadowTex, &rtLightTex}) if (*t) SDL_ReleaseGPUTexture(dev, *t), *t = nullptr;
            SDL_GPUTextureCreateInfo ti{};
            ti.type = SDL_GPU_TEXTURETYPE_2D, ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
            ti.width = hw, ti.height = hh, ti.layer_count_or_depth = 1, ti.num_levels = 1;
            rtShadowTex = SDL_CreateGPUTexture(dev, &ti);
            rtLightTex = SDL_CreateGPUTexture(dev, &ti);
            rtShadowW = hw, rtShadowH = hh;
        }
        float ru[44] = {};
        std::memcpy(ru, inv.data(), sizeof(float) * 16);
        ru[16] = zl[0], ru[17] = zl[1], ru[18] = 262144.0f * 2.0f * 1.7f, ru[19] = float(aoFrame % 4096u);
        ru[20] = eye[0], ru[21] = eye[1], ru[22] = eye[2];
        ru[24] = sunDirForAO[0], ru[25] = sunDirForAO[1], ru[26] = sunDirForAO[2], ru[27] = game::kRtSunConeTan;
        for (size_t k = 0; k < 2 && enh.rtShadows && k < bakedShadowLights.size(); ++k) {
            const game::MapLight& l = *bakedShadowLights[k];
            for (int a = 0; a < 3; ++a) ru[28 + k * 4 + size_t(a)] = l.pos[a];
            ru[28 + k * 4 + 3] = l.radius;
            ru[36 + k] = (l.color[0] + l.color[1] + l.color[2]) / 3.0f;
        }
        ru[40] = 1.0f / float(hw), ru[41] = 1.0f / float(hh), ru[42] = float(hw), ru[43] = float(hh);
        float rflags[4] = {enh.rtShadows && scene.sunColor.x + scene.sunColor.y + scene.sunColor.z > 0.0f ? 1.0f : 0.0f,
                           enh.rtContactAO ? game::kRtContactRange : 0.0f, game::kRtLampSize, 0.0f};
        float full[68] = {};
        std::memcpy(full, ru, sizeof ru);
        std::memcpy(full + 44, rflags, sizeof rflags);
        const bool rtLights = rtDlightsNow();
        for (int k = 0; rtLights && k < kLightShadowMax && size_t(k) < dlights.size(); ++k) { // the effect lights (first four)
            for (int a = 0; a < 3; ++a) full[48 + k * 4 + a] = dlights[size_t(k)].pos[a];
            full[48 + k * 4 + 3] = dlights[size_t(k)].radius;
        }
        full[64] = game::kRtEffectLightSize;
        full[66] = game::kPlayerBodyShadowFadeNear, full[67] = game::kPlayerBodyShadowFadeFar;
        full[65] = rtGINow ? game::kRtGiOcclusionRange : 0.0f; // global illumination: the occlusion reaches further
        if (rtShadowTex && rtLightTex && sceneRT.shadowPass(dev, cmd, floatzTex, rtShadowTex, rtLightTex, hw, hh, full)) {
            u.rt[0] = 1.0f;
            rtLightsReady = rtLights;
        }
    }
    auto pass = [&](SDL_GPUGraphicsPipeline* pipe, SDL_GPUTexture* dst, SDL_GPUTexture* src, SDL_GPUSampler* smp, const void* uu, uint32_t usize) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = dst;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(p, pipe);
        const SDL_GPUTextureSamplerBinding b[2] = {{src, smp}, {u.rt[0] > 0.5f ? rtShadowTex : aoWhite, clampSampler}};
        SDL_BindGPUFragmentSamplers(p, 0, b, pipe == enhAoPipe ? 2 : 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, uu, usize);
        SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
        SDL_EndGPURenderPass(p);
    };
    // the last frame's AO image for the temporal filter (enh_shadow_change), when the AO ran in the frame before this one
    aoPrevValid = aoPrevTex && aoLastFrame != ~0u && aoLastFrame + 1 == taaFrame;
    if (aoPrevValid) {
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureLocation src{}, dst{};
        src.texture = aoTex[0], dst.texture = aoPrevTex;
        SDL_CopyGPUTextureToTexture(cp, &src, &dst, hw, hh, 1, false);
        SDL_EndGPUCopyPass(cp);
    }
    pass(enhAoPipe, aoTex[1], floatzTex, pointClampSampler, &u, sizeof u);
    const float bu[4] = {1.0f / float(hw), 1.0f / float(hh), 0, 0};
    pass(enhAoBlurPipe, aoTex[0], aoTex[1], pointClampSampler, bu, sizeof bu);
    aoReady = true;
    aoLastFrame = taaFrame;
    if (rtGINow) {
        prepareSceneRT(cmd, scene, *frameItems);
        enhRTGI(cmd, hw, hh, inv, zl.data(), eye, vp);
    }
    // the frame's slot-1 uniforms with the frame size, pushed again now that the AO image exists
    enhFragPending.aoParams[0] = 1.0f / float(w), enhFragPending.aoParams[1] = 1.0f / float(h);
    enhFragPending.giParams[0] = giReady ? game::kRtGiStrength : 0.0f;
    SDL_PushGPUFragmentUniformData(cmd, 1, &enhFragPending, sizeof enhFragPending);
}

// Ray-traced global illumination (docs/render-enhanced.md "Ray-traced global illumination"): one ray a frame per half-size pixel
// against the moving objects, the radiance of a hit from the previous frame's image; a depth-aware filter and the accumulation over
// the frames (reprojected by the camera's motion) make the image the scene shaders add to the baked light.
void Renderer::enhRTGI(SDL_GPUCommandBuffer* cmd, uint32_t hw, uint32_t hh, const game::Mat4c& inv, const float zl[2], const float eye[3], const Mat4& vp) {
    giReady = false;
    SDL_GPUTexture* hist = taaHist[taaCur];
    if (!enhGiResolvePipe || !hist || !taaHistValid || !taaPrevVPValid || taaW != outW || taaH != outH || !floatzTex) return;
    if (!giTex[0] || giW != hw || giH != hh) {
        for (SDL_GPUTexture** t : {&giTex[0], &giTex[1], &giRaw}) if (*t) SDL_ReleaseGPUTexture(dev, *t), *t = nullptr;
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D, ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        ti.width = hw, ti.height = hh, ti.layer_count_or_depth = 1, ti.num_levels = 1;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        giTex[0] = SDL_CreateGPUTexture(dev, &ti), giTex[1] = SDL_CreateGPUTexture(dev, &ti);
        ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        giRaw = SDL_CreateGPUTexture(dev, &ti);
        giW = hw, giH = hh, giLastFrame = ~0ull;
    }
    if (!giTex[0] || !giTex[1] || !giRaw) return;
    const bool histValid = giLastFrame == frameCounter - 1 && giScene == sceneRTFor;
    float u[64] = {};
    std::memcpy(u, inv.data(), sizeof(float) * 16);
    u[16] = zl[0], u[17] = zl[1], u[18] = 262144.0f * 2.0f * 1.7f, u[19] = float(aoFrame % 4096u);
    u[20] = eye[0], u[21] = eye[1], u[22] = eye[2];
    std::memcpy(u + 24, vp.data(), sizeof(float) * 16);
    std::memcpy(u + 40, taaPrevVP.data(), sizeof(float) * 16);
    u[56] = 1.0f / float(hw), u[57] = 1.0f / float(hh), u[58] = float(hw), u[59] = float(hh);
    u[60] = game::kRtGiRange, u[61] = game::kRtGiRadianceCap, u[62] = 1.0f;
    if (!sceneRT.giPass(dev, cmd, floatzTex, hist, giRaw, hw, hh, u)) return;
    struct { float inv[16], prev[16], z[4], size[4], params[4]; } ru{};
    std::memcpy(ru.inv, inv.data(), sizeof ru.inv);
    std::memcpy(ru.prev, taaPrevVP.data(), sizeof ru.prev);
    ru.z[0] = zl[0], ru.z[1] = zl[1];
    ru.size[0] = 1.0f / float(hw), ru.size[1] = 1.0f / float(hh);
    ru.params[0] = game::kRtGiBlend, ru.params[1] = histValid ? 1.0f : 0.0f;
    const int dst = 1 - giCur;
    SDL_GPUColorTargetInfo ct{};
    ct.texture = giTex[dst], ct.load_op = SDL_GPU_LOADOP_DONT_CARE, ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhGiResolvePipe);
    const SDL_GPUTextureSamplerBinding b[2] = {{giRaw, pointClampSampler}, {histValid ? giTex[giCur] : giRaw, clampSampler}};
    SDL_BindGPUFragmentSamplers(p, 0, b, 2);
    SDL_PushGPUFragmentUniformData(cmd, 0, &ru, sizeof ru);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    giCur = dst, giLastFrame = frameCounter, giScene = sceneRTFor, giReady = true;
    static bool logged = false;
    if (!logged) logged = true, std::cout << "ray-traced global illumination: " << hw << "x" << hh << ", range " << game::kRtGiRange << " units\n";
}

// Point light shadows: per light (first kLightShadowMax), six perspective faces (game::cubeFace axes) into a 3 x 2 tile block of the
// atlas; casters are the light's lit world pieces (cullWorld) and the model items whose box the light reaches.
void Renderer::renderLightShadows(SDL_GPUCommandBuffer* cmd, const Scene& scene, const std::vector<DrawItem>& items) {
    const int S = kLightShadowSize;
    if (!lightShadowAtlas) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = uint32_t(3 * S), ti.height = uint32_t(2 * S * kLightShadowRows), ti.layer_count_or_depth = 1, ti.num_levels = 1;
        lightShadowAtlas = SDL_CreateGPUTexture(dev, &ti);
        if (!lightShadowAtlas) return;
    }
    const int n = rtDlightsNow() ? 0 : std::min<int>(int(dlights.size()), kLightShadowMax); // (ray-traced: the AO pass)
    struct ShadowLight { float pos[3]; float radius; int row; bool world; int dl; };
    std::vector<ShadowLight> lights;
    for (int li = 0; li < n; ++li) {
        const DLight& dl = dlights[size_t(li)];
        lights.push_back({{dl.pos[0], dl.pos[1], dl.pos[2]}, dl.radius, li, true, li});
    }
    for (size_t k = 0; k < bakedShadowLights.size() && !(rtShadowNow && enh.rtShadows); ++k) { // (ray-traced instead: the AO pass)
        const game::MapLight& ml = *bakedShadowLights[k];
        lights.push_back({{ml.pos[0], ml.pos[1], ml.pos[2]}, ml.radius, kLightShadowMax + int(k), false, -1});
    }
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = lightShadowAtlas;
    dt.clear_depth = 1.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* sp = SDL_BeginGPURenderPass(cmd, nullptr, 0, &dt);
    SDL_BindGPUGraphicsPipeline(sp, enhShadowPipe);
    struct SVU { float vp[16]; Mat4 model; float p[4]; };
    for (const ShadowLight& dl : lights) {
        const int li = dl.row;
        const float nr = lightShadowNear, fr = std::max(dl.radius, nr * 2.0f);
        std::vector<std::pair<const DrawItem*, int>> near; // items within the light's reach
        for (const DrawItem& it : items) {
            if (!it.model || it.effects || it.depthMax < 1.0f || !it.model->hasBounds) continue;
            float mn[3], mx[3];
            game::transformedBoxBounds(it.model->mins, it.model->maxs, it.transform.data(), mn, mx);
            if (game::lightTouchesBox(dl.pos, dl.radius, mn, mx)) near.push_back({&it, 0});
        }
        for (int f = 0; f < 6; ++f) {
            const game::CubeFace cf = game::cubeFace(f);
            const float A = fr / (fr - nr), B = -fr * nr / (fr - nr);
            auto d3 = [&](const float* v) { return v[0] * dl.pos[0] + v[1] * dl.pos[1] + v[2] * dl.pos[2]; };
            SVU u{};
            for (int k = 0; k < 3; ++k) {
                u.vp[k * 4 + 0] = cf.right[k];
                u.vp[k * 4 + 1] = cf.up[k];
                u.vp[k * 4 + 2] = A * cf.fwd[k];
                u.vp[k * 4 + 3] = cf.fwd[k];
            }
            u.vp[12] = -d3(cf.right), u.vp[13] = -d3(cf.up), u.vp[14] = -A * d3(cf.fwd) + B, u.vp[15] = -d3(cf.fwd);
            const SDL_GPUViewport vp{float((f % 3) * S), float((f / 3 + 2 * li) * S), float(S), float(S), 0.0f, 1.0f};
            SDL_SetGPUViewport(sp, &vp);
            if (dl.world && culledIndexBuf && size_t(dl.dl) < culledLightBatches.size()) { // baked lights: moving casters only
                const SDL_GPUBufferBinding vb{scene.vertices, 0};
                SDL_BindGPUVertexBuffers(sp, 0, &vb, 1);
                const SDL_GPUBufferBinding ib{culledIndexBuf, 0};
                SDL_BindGPUIndexBuffer(sp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                u.model = kIdentity;
                for (const Batch& b : culledLightBatches[size_t(dl.dl)]) {
                    if (!b.indexCount || (b.pass != Pass::Opaque && b.pass != Pass::AlphaTest)) continue;
                    u.p[0] = b.pass == Pass::AlphaTest ? 1.0f : 0.0f;
                    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
                    const SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
                    SDL_BindGPUFragmentSamplers(sp, 0, &tb, 1);
                    drawIndexed(sp, b.indexCount, 1, b.firstIndex, 0, 0);
                }
            }
            for (const auto& [it, unused] : near) {
                (void)unused;
                const SDL_GPUBufferBinding vb{it->model->vertices, it->model->vertexOffset};
                SDL_BindGPUVertexBuffers(sp, 0, &vb, 1);
                const SDL_GPUBufferBinding ib{it->model->indices, 0};
                SDL_BindGPUIndexBuffer(sp, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                u.model = it->transform;
                for (const Batch& b : it->model->batches) {
                    if ((b.pass != Pass::Opaque && b.pass != Pass::AlphaTest) || b.fxKind != 0 || !b.indexCount) continue;
                    u.p[0] = b.pass == Pass::AlphaTest ? 1.0f : 0.0f;
                    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
                    const SDL_GPUTextureSamplerBinding tb{scene.textures[size_t(b.texture)], matSampler(b.colorState)};
                    SDL_BindGPUFragmentSamplers(sp, 0, &tb, 1);
                    drawIndexed(sp, b.indexCount, 1, b.firstIndex, 0, 0);
                }
            }
        }
    }
    SDL_EndGPURenderPass(sp);
    lightShadowsNow = n;
}

// Sun shafts (docs/render-enhanced.md "Atmosphere"): a 16-step march per half-size pixel through the sun's shadow (world cascades
// and moving casters) up to the opaque depth, Henyey-Greenstein phase, added to the scene before TAA (which smooths the noise).
void Renderer::enhVolume(SDL_GPUCommandBuffer* cmd, uint32_t w, uint32_t h, const Scene& scene, const Camera& cam) {
    if (scene.sunColor.x + scene.sunColor.y + scene.sunColor.z <= 0.0f || (worldCascadesNow == 0 && shadowCascadesNow == 0)) return;
    const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
    if (!volTex || volW != hw || volH != hh) {
        if (volTex) SDL_ReleaseGPUTexture(dev, volTex);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = hw, ti.height = hh, ti.layer_count_or_depth = 1, ti.num_levels = 1;
        volTex = SDL_CreateGPUTexture(dev, &ti);
        if (!volTex) return;
        volW = hw, volH = hh;
    }
    const Mat4 vp = cam.viewProj(frameAspect());
    game::Mat4c cur{}, inv{};
    std::memcpy(cur.data(), vp.data(), sizeof(float) * 16);
    if (!game::invertMat(cur, inv)) return;
    const Vec3 fw = cam.forward();
    const float eye[3] = {cam.pos.x, cam.pos.y, cam.pos.z}, fwd[3] = {fw.x, fw.y, fw.z};
    const auto zl = game::clipDepthLine(cur, eye, fwd);
    struct { float inv[16], z[4], eye[4], sun[4], color[4]; } v{};
    std::memcpy(v.inv, inv.data(), sizeof v.inv);
    v.z[0] = zl[0], v.z[1] = zl[1], v.z[2] = 262144.0f * 2.0f * 1.7f, v.z[3] = float(taaFrame % 64u);
    v.eye[0] = eye[0], v.eye[1] = eye[1], v.eye[2] = eye[2], v.eye[3] = 1500.0f;
    // the air scatters more on maps with fog (the level's own fog says the air is hazy there)
    v.sun[0] = scene.sunDirection.x, v.sun[1] = scene.sunDirection.y, v.sun[2] = scene.sunDirection.z,
    v.sun[3] = enh.volumetric * (fog.active ? 0.06f : 0.025f);
    v.color[0] = scene.sunColor.x, v.color[1] = scene.sunColor.y, v.color[2] = scene.sunColor.z, v.color[3] = 0.6f;
    SDL_GPUColorTargetInfo ct{};
    ct.texture = volTex;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhVolumePipe);
    const SDL_GPUTextureSamplerBinding tb[3] = {{frameDepth, pointClampSampler},
                                                {shadowCascadesNow > 0 ? shadowTex : shadowDummy, shadowCmpSampler},
                                                {worldCascadesNow > 0 ? worldShadowArray : shadowDummyArray, shadowCmpSampler}};
    SDL_BindGPUFragmentSamplers(p, 0, tb, 3);
    SDL_PushGPUFragmentUniformData(cmd, 0, &enhFragPending, sizeof enhFragPending);
    SDL_PushGPUFragmentUniformData(cmd, 1, &v, sizeof v);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    SDL_PushGPUFragmentUniformData(cmd, 1, &enhFragPending, sizeof enhFragPending); // slot 1 belongs to the scene shaders again
    SDL_GPUColorTargetInfo at{};
    at.texture = sceneTex;
    at.load_op = SDL_GPU_LOADOP_LOAD;
    at.store_op = SDL_GPU_STOREOP_STORE;
    p = SDL_BeginGPURenderPass(cmd, &at, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhVolumeApplyPipe);
    const SDL_GPUTextureSamplerBinding vb{volTex, clampSampler};
    SDL_BindGPUFragmentSamplers(p, 0, &vb, 1);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
}

// Depth of field while aiming: into the free history image, copied back into the scene image.
void Renderer::enhDof(SDL_GPUCommandBuffer* cmd, uint32_t w, uint32_t h, const Camera& cam) {
    SDL_GPUTexture* scratch = taaHist[1 - taaCur];
    if (!scratch || !frameDepth) return;
    game::Mat4c cur{};
    const Mat4 vp = cam.viewProj(float(w) / float(h));
    std::memcpy(cur.data(), vp.data(), sizeof(float) * 16);
    const Vec3 fw = cam.forward();
    const float eye[3] = {cam.pos.x, cam.pos.y, cam.pos.z}, fwd[3] = {fw.x, fw.y, fw.z};
    const auto zl = game::clipDepthLine(cur, eye, fwd);
    const float u[4] = {zl[0], zl[1], adsFrac * enh.dof, 262144.0f * 2.0f * 1.7f};
    SDL_GPUColorTargetInfo ct{};
    ct.texture = scratch;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhDofPipe);
    const SDL_GPUTextureSamplerBinding tb[2] = {{sceneTex, clampSampler}, {frameDepth, pointClampSampler}};
    SDL_BindGPUFragmentSamplers(p, 0, tb, 2);
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof u);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation src{}, dst{};
    src.texture = scratch;
    dst.texture = sceneTex;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, w, h, 1, false);
    SDL_EndGPUCopyPass(cp);
}

// Motion blur along the TAA motion vectors, into the free history image, copied back into the scene image.
void Renderer::enhMotionBlur(SDL_GPUCommandBuffer* cmd, uint32_t w, uint32_t h) {
    SDL_GPUTexture* scratch = taaHist[1 - taaCur]; // the old history: free until the next resolve writes it
    if (!scratch || !velocityTex) return;
    SDL_GPUColorTargetInfo ct{};
    ct.texture = scratch;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(p, enhMotionBlurPipe);
    const SDL_GPUTextureSamplerBinding tb[2] = {{sceneTex, clampSampler}, {velocityTex, pointClampSampler}};
    SDL_BindGPUFragmentSamplers(p, 0, tb, 2);
    const float u[4] = {enh.motionBlur, 0, 0, 0};
    SDL_PushGPUFragmentUniformData(cmd, 0, u, sizeof u);
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureLocation src{}, dst{};
    src.texture = scratch;
    dst.texture = sceneTex;
    SDL_CopyGPUTextureToTexture(cp, &src, &dst, w, h, 1, false);
    SDL_EndGPUCopyPass(cp);
}

// A copy of the sky cube with a mip chain (box-filtered), so rough surfaces reflect a blurred sky.
void Renderer::ensureEnvCube(SDL_GPUCommandBuffer* cmd, const Scene& scene) {
    if (capturingProbes || (probeCount > 0 && probesFor == scene.serial)) return; // the probe array holds the sky as cube 0
    if (envFor == scene.sky && (envCube || !scene.sky)) return;
    if (envCube) SDL_ReleaseGPUTexture(dev, envCube), envCube = nullptr;
    probeCount = 0;
    envFor = scene.sky;
    if (!scene.sky || !enh.ggx) return;
    // the sky's size is not kept with the texture: read it back from the image record at load (Scene::skySize)
    const uint32_t size = scene.skySize;
    if (size == 0) return;
    envLevels = 1;
    for (uint32_t s = size; s > 1; s /= 2) ++envLevels;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = ti.height = size;
    ti.layer_count_or_depth = 6;
    ti.num_levels = uint32_t(envLevels);
    envCube = SDL_CreateGPUTexture(dev, &ti);
    if (!envCube) return;
    for (uint32_t face = 0; face < 6; ++face) {
        SDL_GPUBlitInfo bi{};
        bi.source.texture = scene.sky;
        bi.source.layer_or_depth_plane = face;
        bi.source.w = bi.source.h = size;
        bi.destination.texture = envCube;
        bi.destination.layer_or_depth_plane = face;
        bi.destination.w = bi.destination.h = size;
        bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        bi.filter = SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(cmd, &bi);
    }
    SDL_GenerateMipmapsForGPUTexture(cmd, envCube);
}

void Renderer::dumpProbes(const char* dump) { // debug IW2_PROBE_DUMP: the sky and the first probes, six faces each, as a PPM
    constexpr uint32_t kProbeSize = 64;
    const int n = probeCount;
    {
        const uint32_t cubes = uint32_t(std::min(n + 1, 8)), W = 6 * kProbeSize, H = cubes * kProbeSize;
        SDL_GPUTextureCreateInfo di{};
        di.type = SDL_GPU_TEXTURETYPE_2D, di.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, di.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        di.width = W, di.height = H, di.layer_count_or_depth = 1, di.num_levels = 1;
        SDL_GPUTexture* sheet = SDL_CreateGPUTexture(dev, &di);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, tbi.size = W * H * 4;
        SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
        for (uint32_t k = 0; k < cubes; ++k)
            for (uint32_t f = 0; f < 6; ++f) {
                SDL_GPUBlitInfo bi{};
                bi.source.texture = envCube, bi.source.layer_or_depth_plane = 6 * k + f, bi.source.w = bi.source.h = kProbeSize;
                bi.destination.texture = sheet, bi.destination.x = f * kProbeSize, bi.destination.y = k * kProbeSize;
                bi.destination.w = bi.destination.h = kProbeSize;
                bi.load_op = SDL_GPU_LOADOP_LOAD, bi.filter = SDL_GPU_FILTER_NEAREST;
                SDL_BlitGPUTexture(cmd, &bi);
            }
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion rg{};
        rg.texture = sheet, rg.w = W, rg.h = H, rg.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb;
        SDL_DownloadFromGPUTexture(cp, &rg, &dst);
        SDL_EndGPUCopyPass(cp);
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        SDL_WaitForGPUFences(dev, true, &fence, 1);
        SDL_ReleaseGPUFence(dev, fence);
        const auto* px = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(dev, tb, false));
        if (FILE* fo = std::fopen(dump, "wb")) {
            std::fprintf(fo, "P6\n%u %u\n255\n", W, H);
            for (uint32_t i = 0; i < W * H; ++i) std::fwrite(px + i * 4, 1, 3, fo);
            std::fclose(fo);
        }
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTexture(dev, sheet);
    }
}

// Reflection probes (docs/render-enhanced.md "Reflection probes"): placed on the light grid's points (playable space), at most
// kProbeMax, one per portal cell first; each is six 64 x 64 faces of the scene (the lightmapped world, static models and the sky,
// no moving models, effects or dynamic lights; the post passes off) drawn through drawFrame and copied out of the linear scene
// target into the face array (cube 0 the sky). The index grid gives each 128-unit cell its probe (game::assignProbes).
void Renderer::captureProbes(SDL_GPUCommandBuffer* cmd, const Scene& scene) {
    constexpr size_t kProbeMax = 64;
    constexpr uint32_t kProbeSize = 64;
    probesFor = scene.serial;
    probeCount = 0;
    if (probeIdxBuf) SDL_ReleaseGPUBuffer(dev, probeIdxBuf), probeIdxBuf = nullptr;
    if (!scene.grid || scene.grid->empty() || !scene.vis.valid() || scene.batches.empty()) return;
    const uint64_t t0 = SDL_GetTicksNS();
    std::vector<std::array<float, 3>> pts = game::pointsNearBoxes(scene.grid->points(), scene.soupBoxes, 128.0f, 4096.0f);
    if (pts.empty()) return;
    if (const size_t stride = pts.size() / 40000 + 1; stride > 1) { // thin very dense grids (placement is points x probes)
        size_t n = 0;
        for (size_t i = 0; i < pts.size(); i += stride) pts[n++] = pts[i];
        pts.resize(n);
    }
    std::vector<int> ptCell(pts.size());
    for (size_t i = 0; i < pts.size(); ++i) ptCell[i] = game::cellForPoint(scene.vis, pts[i].data());
    const std::vector<int> pick = game::placeProbes(pts, ptCell, kProbeMax);
    if (pick.empty()) return;
    std::vector<std::array<float, 3>> pos;
    std::vector<int> pcell;
    for (const int i : pick) pos.push_back(pts[size_t(i)]), pcell.push_back(ptCell[size_t(i)]);
    if (const char* at = std::getenv("IW2_PROBE_AT")) // debug: probe 0 at x,y,z (e.g. above the map, to compare its faces with the sky)
        std::sscanf(at, "%f,%f,%f", &pos[0][0], &pos[0][1], &pos[0][2]);
    // The index grid covers the playable space (the grid points' box, 256 units more on each side); outside it, the sky.
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (const auto& p : pts)
        for (int a = 0; a < 3; ++a) mn[a] = std::min(mn[a], p[size_t(a)] - 256.0f), mx[a] = std::max(mx[a], p[size_t(a)] + 256.0f);
    probeGrid = game::probeGridInfo(mn, mx, 128.0f, size_t(1) << 20);
    std::vector<std::vector<int>> next(scene.vis.cells.size());
    for (size_t c = 0; c < scene.vis.cells.size(); ++c) {
        const game::VisCell& vc = scene.vis.cells[c];
        for (int p = vc.firstPortal; p < vc.firstPortal + vc.portalCount && p < int(scene.vis.portals.size()); ++p)
            if (p >= 0) next[c].push_back(scene.vis.portals[size_t(p)].cell);
    }
    auto adjacent = [&](int a, int b) {
        for (const int x : next[size_t(a)]) if (x == b) return true;
        for (const int x : next[size_t(b)]) if (x == a) return true;
        return false;
    };
    std::vector<uint8_t> idx = game::assignProbes(probeGrid, pos, pcell, [&](const float p[3]) { return game::cellForPoint(scene.vis, p); }, adjacent);
    idx.resize((idx.size() + 3) / 4 * 4, 0);
    {
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        bi.size = uint32_t(idx.size());
        probeIdxBuf = SDL_CreateGPUBuffer(dev, &bi);
        if (!probeIdxBuf) return;
        Uploader up(dev);
        up.buffer(probeIdxBuf, idx.data(), uint32_t(idx.size()));
    }
    const int n = int(pos.size());
    if (envCube) SDL_ReleaseGPUTexture(dev, envCube), envCube = nullptr;
    envLevels = 1;
    for (uint32_t s = kProbeSize; s > 1; s /= 2) ++envLevels;
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = ti.height = kProbeSize;
    ti.layer_count_or_depth = uint32_t(6 * (n + 1));
    ti.num_levels = uint32_t(envLevels);
    envCube = SDL_CreateGPUTexture(dev, &ti);
    if (!envCube) { SDL_ReleaseGPUBuffer(dev, probeIdxBuf), probeIdxBuf = nullptr; return; }
    envFor = scene.sky;
    // Everything goes into the frame's command buffer, before the frame itself: it carries the uniforms the caller pushed for it.
    for (uint32_t face = 0; face < 6; ++face) { // cube 0: the sky (black without one)
        SDL_GPUBlitInfo bi{};
        if (scene.sky && scene.skySize > 0) {
            bi.source.texture = scene.sky;
            bi.source.layer_or_depth_plane = face;
            bi.source.w = bi.source.h = scene.skySize;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
        } else {
            bi.source.texture = envDummy;
            bi.source.layer_or_depth_plane = face;
            bi.source.w = bi.source.h = 1;
        }
        bi.destination.texture = envCube;
        bi.destination.layer_or_depth_plane = face;
        bi.destination.w = bi.destination.h = kProbeSize;
        bi.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(cmd, &bi);
    }
    SDL_GenerateMipmapsForGPUTexture(cmd, envCube); // the captures reflect cube 0 (rough surfaces read its mips)
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = colorFormat;
    ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = ci.height = kProbeSize;
    ci.layer_count_or_depth = 1, ci.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(dev, &ci);
    const game::EnhancedSettings saved = enh;
    const std::vector<DLight> savedLights = dlights;
    enh.taa = false, enh.autoExposure = false, enh.bloom = false, enh.volumetric = 0.0f, enh.motionBlur = 0.0f, enh.dof = 0.0f;
    enh.grade = 0, enh.ssr = false, enh.ao = false, enh.contactShadows = false, enh.sunShadows = 0, enh.worldShadows = 0;
    enh.lightShadows = false;
    dlights.clear();
    capturingProbes = true;
    // Face k of a cube: forward and right as the cube's face axes. The copy flips the image vertically except for the +y / -y faces:
    // that orientation reproduces the sky cube's own faces exactly from a probe above the map (IW2_PROBE_AT, IW2_PROBE_DUMP).
    static const float kFace[6][2][3] = {{{1, 0, 0}, {0, 0, -1}}, {{-1, 0, 0}, {0, 0, 1}}, {{0, 1, 0}, {1, 0, 0}},
                                         {{0, -1, 0}, {1, 0, 0}}, {{0, 0, 1}, {1, 0, 0}}, {{0, 0, -1}, {-1, 0, 0}}};
    Camera c;
    c.hasBasis = true;
    c.fovX43 = 2.0f * std::atan(4.0f / 3.0f) * 180.0f / float(M_PI); // 90 degrees vertically (viewProj takes the 4:3 width)
    const std::vector<DrawItem> noItems;
    const std::vector<Quad2D> noOverlay;
    for (int k = 0; target && k < n; ++k) {
        for (int f = 0; f < 6; ++f) {
            c.pos = {pos[size_t(k)][0], pos[size_t(k)][1], pos[size_t(k)][2]};
            c.basisF = {kFace[f][0][0], kFace[f][0][1], kFace[f][0][2]};
            c.basisR = {kFace[f][1][0], kFace[f][1][1], kFace[f][1][2]};
            drawFrame(cmd, target, kProbeSize, kProbeSize, scene, c, noItems, noOverlay, 0.0f);
            SDL_GPUBlitInfo bi{};
            bi.source.texture = sceneTex; // the linear scene before the tone pass
            bi.source.w = bi.source.h = kProbeSize;
            bi.destination.texture = envCube;
            bi.destination.layer_or_depth_plane = uint32_t(6 * (k + 1) + f);
            bi.destination.w = bi.destination.h = kProbeSize;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.flip_mode = f == 2 || f == 3 ? SDL_FLIP_NONE : SDL_FLIP_VERTICAL; // measured against the sky cube's own faces
            bi.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cmd, &bi);
        }
    }
    SDL_GenerateMipmapsForGPUTexture(cmd, envCube);
    if (target) SDL_ReleaseGPUTexture(dev, target);
    enh = saved;
    dlights = savedLights;
    capturingProbes = false;
    taaHistValid = false, taaPrevVPValid = false;
    for (WorldCascade& wc : worldCascades) wc.valid = false;
    probeCount = target ? n : 0;
    std::cout << "reflection probes: " << probeCount << " in " << (SDL_GetTicksNS() - t0) / 1000000ull << " ms (grid " << probeGrid.dims[0]
              << " x " << probeGrid.dims[1] << " x " << probeGrid.dims[2] << ", " << probeGrid.cell << " units)\n";
}

void Renderer::enhPost(SDL_GPUCommandBuffer* cmd, uint32_t w, uint32_t h, const Camera& cam, bool floatzReady) {
    auto pass = [&](SDL_GPUGraphicsPipeline* pipe, SDL_GPUTexture* dst, bool load, const SDL_GPUTextureSamplerBinding* src, uint32_t nsrc,
                    const void* u, uint32_t usize) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = dst;
        ct.load_op = load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* p = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(p, pipe);
        SDL_BindGPUFragmentSamplers(p, 0, src, nsrc);
        if (usize) SDL_PushGPUFragmentUniformData(cmd, 0, u, usize);
        SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
        SDL_EndGPURenderPass(p);
    };
    if (enh.autoExposure) {
        const SDL_GPUTextureSamplerBinding sb{sceneTex, clampSampler};
        pass(enhLumPipe, lumTex, false, &sb, 1, nullptr, 0);
        SDL_GenerateMipmapsForGPUTexture(cmd, lumTex);
        const game::ExposureParams ep;
        const bool reset = !expoValid || renderTimeMs < expoLastMs;
        const float dt = reset ? 0.0f : std::min(float(renderTimeMs - expoLastMs) * 0.001f, 0.25f);
        const float u[12] = {ep.keyLog2, ep.strength, ep.minEv, ep.maxEv, dt, ep.tauBrighten, ep.tauDarken, reset ? 1.0f : 0.0f,
                             float(lumLevels - 1), 0, 0, 0};
        const SDL_GPUTextureSamplerBinding eb[2] = {{lumTex, clampSampler}, {expoTex[expoCur], pointClampSampler}};
        pass(enhExpoPipe, expoTex[1 - expoCur], false, eb, 2, u, sizeof u);
        expoCur = 1 - expoCur;
        expoValid = true;
        expoLastMs = renderTimeMs;
    }
    // Camera motion vectors (docs/render-enhanced.md "Motion vectors"); for now only the debug view consumes them.
    const Mat4 vp = cam.viewProj(float(w) / float(h));
    if (enh.motionDebug == 1 && floatzReady && !velocityFromFloatz) {
        if (!velocityTex || velocityW != w || velocityH != h) {
            if (velocityTex) SDL_ReleaseGPUTexture(dev, velocityTex);
            SDL_GPUTextureCreateInfo ti{};
            ti.type = SDL_GPU_TEXTURETYPE_2D;
            ti.format = kVelocityFormat;
            ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ti.width = w, ti.height = h, ti.layer_count_or_depth = 1, ti.num_levels = 1;
            velocityTex = SDL_CreateGPUTexture(dev, &ti);
            if (!velocityTex) sdlFail("velocity texture");
            velocityW = w, velocityH = h;
        }
        game::Mat4c cur{}, inv{};
        std::memcpy(cur.data(), vp.data(), sizeof(float) * 16);
        if (game::invertMat(cur, inv)) {
            const Vec3 f = cam.forward();
            const float eye[3] = {cam.pos.x, cam.pos.y, cam.pos.z}, fwd[3] = {f.x, f.y, f.z};
            const auto zl = game::clipDepthLine(cur, eye, fwd);
            struct { float inv[16], prev[16], z[4]; } vu{};
            std::memcpy(vu.inv, inv.data(), sizeof vu.inv);
            Mat4 prev = hasPrevViewProj ? prevViewProj : vp;
            if (const char* py = std::getenv("IW2_ENH_PREV_CAM")) prev = debugPrevCamViewProj(cam, py, float(w) / float(h)); // debug (--shot)
            std::memcpy(vu.prev, prev.data(), sizeof vu.prev);
            vu.z[0] = zl[0], vu.z[1] = zl[1], vu.z[2] = 262144.0f * 2.0f * 1.7f; // below the $floatz sky clear (build floatz)
            const SDL_GPUTextureSamplerBinding fb{floatzTex, pointClampSampler};
            pass(enhVelocityPipe, velocityTex, false, &fb, 1, &vu, sizeof vu);
        }
    }
    prevViewProj = vp;
    hasPrevViewProj = true;
    if (enh.bloom) {
        ensureBloomTargets(w, h);
        const int n = int(bloomTex.size());
        float u[8] = {1.0f / float(w), 1.0f / float(h), 1.0f, 0.5f, enh.exposureBias, enh.autoExposure ? 1.0f : 0.0f, 0, 0};
        const SDL_GPUTextureSamplerBinding pb[2] = {{sceneTex, clampSampler}, {expoTex[expoCur], pointClampSampler}};
        pass(enhBloomPrePipe, bloomTex[0], false, pb, 2, u, sizeof u);
        for (int i = 1; i < n; ++i) {
            u[0] = 1.0f / float(bloomSize[size_t(i - 1)][0]), u[1] = 1.0f / float(bloomSize[size_t(i - 1)][1]);
            const SDL_GPUTextureSamplerBinding db{bloomTex[size_t(i - 1)], clampSampler};
            pass(enhBloomDownPipe, bloomTex[size_t(i)], false, &db, 1, u, sizeof u);
        }
        for (int i = n - 2; i >= 0; --i) {
            u[0] = 1.0f / float(bloomSize[size_t(i + 1)][0]), u[1] = 1.0f / float(bloomSize[size_t(i + 1)][1]);
            const SDL_GPUTextureSamplerBinding ub{bloomTex[size_t(i + 1)], clampSampler};
            pass(enhBloomUpPipe, bloomTex[size_t(i)], true, &ub, 1, u, sizeof u);
        }
    }
}
