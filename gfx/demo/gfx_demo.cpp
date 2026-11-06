// gfx_demo -- standalone viewer for the gfx module.
//
//   ./gfx_demo [--model NAME] [--skybox NAME] [--screenshot out.png]
//              [--angle DEG] [--dist M] [--height M] [--size WxH] [--no-shadows]
//              [--sky-only] [--fov DEG] [--look YAW,PITCH]
//              --hud [--hud-theme NAME] [--hud-scale S] [--night 0|1]
//              --fx [--time T] [--nitro 0|1] [--slip 0|1] [--no-blur] [--no-lines] [--no-flames] [--low]
//
// --fx drives an AE86 down an endless road behind a chase camera with nitro
// bursts (flames, speed FOV, motion + radial blur, speed lines) and an RX7
// ahead to slipstream behind. --time starts the simulation at T seconds and
// (with --screenshot) steps it at a fixed 30 Hz so shots are reproducible.
//
// Shows a ground plane, the cars (ae86, rx7 and an ae86 assembled from
// ae86_body + 4x ae86_wheel using the converter's wheel anchors), two rings of
// trees, tyre-smoke billboards and the skybox, with an orbiting camera.
// --model NAME puts just that model in the middle. --screenshot renders a few
// frames into a hidden window, saves a PNG and exits. Keys: 1-9/0 cycle
// skyboxes, M cycles the centre model, Esc quits.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx/include/hud_gauges.h"
#include "gfx/include/renderer.h"

using namespace dm;

static gfx::MeshData makeGround(float half, int n)
{
    gfx::MeshData m;
    float step = 2 * half / n;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            float px = -half + x * step, pz = -half + z * step;
            m.positions.push_back({px, 0, pz});
            m.normals.push_back({0, 1, 0});
            bool road = std::fabs(px) < 4.0f;
            bool check = ((int)std::floor(px / 4) + (int)std::floor(pz / 4)) & 1;
            Vec3 grass = check ? Vec3{0.42f, 0.62f, 0.30f} : Vec3{0.38f, 0.58f, 0.27f};
            m.colors.push_back(road ? Vec3{0.32f, 0.32f, 0.35f} : grass);
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            uint32_t a = z * (n + 1) + x, b = a + 1, c = a + (n + 1), d = c + 1;
            // CCW seen from +Y
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    return m;
}

// --fx: a chase-camera drive down an endless road with nitro bursts every
// 7 s (on between 3.0 and 5.5 s of each cycle), an RX7 ahead to draft behind
// and all screen effects on. Simulation time t is passed in so screenshots
// are deterministic.
struct FxOptions
{
    int forceNitro = -1; // -1 = schedule, 0 = off, 1 = on
    int forceSlip = -1;
    bool lines = true;
    bool blur = true;
    bool flames = true;
};

static float hash1(float n) { return std::fmod(std::fabs(std::sin(n * 12.9898f) * 43758.5453f), 1.0f); }

struct FxState
{
    float z, x, yaw, speed01, nitro01, ignition, slip, rxGap, rxNitro, rxIgnition;
    float wheelAngle;
};

static FxState fxSim(float t, const FxOptions &o)
{
    auto nitroAt = [&](float tt, float &ign) {
        float tc = std::fmod(tt, 7.0f);
        bool on = o.forceNitro >= 0 ? o.forceNitro == 1 : (tc >= 3.0f && tc < 5.5f);
        ign = (on && o.forceNitro < 0) ? std::max(0.0f, 1.0f - (tc - 3.0f) / 0.35f) : 0.0f;
        if (o.forceNitro >= 0) return on ? 1.0f : 0.0f;
        if (on) return std::min(1.0f, (tc - 3.0f) / 0.25f);
        return tc >= 5.5f ? std::max(0.0f, 1.0f - (tc - 5.5f) / 0.4f) : 0.0f;
    };
    FxState s{};
    // integrate distance (speed depends on nitro)
    float dist = 0, dt = 1.0f / 120.0f, dummy;
    for (float tt = 0; tt < t; tt += dt) dist += (34.0f + 10.0f * nitroAt(tt, dummy)) * std::min(dt, t - tt);
    s.nitro01 = nitroAt(t, s.ignition);
    s.speed01 = 0.82f + 0.18f * s.nitro01;
    s.z = -dist;
    s.x = 1.1f * std::sin(t * 0.55f);
    s.yaw = -std::atan2(1.1f * 0.55f * std::cos(t * 0.55f), 34.0f) * 1.0f;
    s.wheelAngle = dist / 0.294f;
    s.rxGap = 12.0f + 7.0f * std::sin(t * 0.33f);
    float tc = std::fmod(t, 7.0f);
    bool rxOn = tc >= 0.5f && tc < 2.0f;
    s.rxNitro = rxOn ? std::min(1.0f, (tc - 0.5f) / 0.2f) : 0.0f;
    s.rxIgnition = rxOn ? std::max(0.0f, 1.0f - (tc - 0.5f) / 0.35f) : 0.0f;
    s.slip = o.forceSlip >= 0 ? (float)o.forceSlip : dm::clampf((17.0f - s.rxGap) / 6.0f, 0.0f, 1.0f);
    return s;
}

// --hud: arcade cluster animation -- revving through the gears with shifts
// and a limiter bounce in 6th, nitro burn / rubber-band recharge cycles.
static void hudSim(float t, gfx::TachState &tach, gfx::NitroGaugeState &n)
{
    float tc = std::fmod(t, 14.0f);
    static const float kmhPer1000[7] = {0, 8.5f, 13.5f, 18.5f, 23.5f, 28.5f, 33.0f};
    tach.maxRpm = 8000;
    tach.redlineRpm = 7200;
    if (tc < 0.7f)
    {
        tach.gear = 0;
        tach.rpm = 950 + 60 * std::sin(t * 30.0f);
        tach.speedKmh = 0;
    }
    else
    {
        float u = tc - 0.7f;
        tach.gear = std::min(6, 1 + (int)(u / 2.2f));
        float p = (u - (tach.gear - 1) * 2.2f) / 2.2f;
        tach.rpm = 3600 + 4300 * std::min(1.0f, p * 1.08f);
        tach.revLimiter = tach.gear == 6 && p > 0.85f;
        if (tach.revLimiter) tach.rpm = 7850 + 150 * std::sin(t * 70.0f);
        tach.speedKmh = tach.rpm / 1000.0f * kmhPer1000[tach.gear];
    }
    tach.time = t;
    float nc = std::fmod(t, 9.0f);
    n.burning = nc >= 2.0f && nc < 4.5f;
    if (nc < 2.0f) n.level01 = 0.55f + 0.45f * nc / 2.0f;
    else if (n.burning) n.level01 = 1.0f - 0.85f * (nc - 2.0f) / 2.5f;
    else n.level01 = 0.15f + 0.4f * (nc - 4.5f) / 4.5f;
    n.rechargeMultiplier = dm::clampf(1.25f + 0.75f * std::sin(t * 0.45f), 0.5f, 2.0f);
    n.slipstream = std::sin(t * 0.7f) > 0.5f;
    n.time = t;
}

int main(int argc, char **argv)
{
    std::string model, skybox = "SkyAfterNoon", shot;
    float angleDeg = -1, dist = 11.0f, camH = 3.0f, fov = 60.0f;
    bool look = false;
    float lookYaw = 0, lookPitch = 0;
    int W = 1280, H = 720;
    bool shadows = true, skyOnly = false, fxMode = false, low = false, hudMode = false;
    int nightMode = -1;
    float hudScale = 1.0f;
    std::string hudTheme = "initiald";
    float fxStart = -1.0f;
    FxOptions fxo;
    int shotFrames = 3;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> const char * { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--model") model = next();
        else if (a == "--skybox") skybox = next();
        else if (a == "--screenshot") shot = next();
        else if (a == "--angle") angleDeg = (float)std::atof(next());
        else if (a == "--dist") dist = (float)std::atof(next());
        else if (a == "--height") camH = (float)std::atof(next());
        else if (a == "--size") std::sscanf(next(), "%dx%d", &W, &H);
        else if (a == "--no-shadows") shadows = false;
        else if (a == "--sky-only") skyOnly = true;
        else if (a == "--fx") fxMode = true;
        else if (a == "--hud") hudMode = true;
        else if (a == "--hud-theme") hudTheme = next();
        else if (a == "--hud-scale") hudScale = (float)std::atof(next());
        else if (a == "--night") nightMode = std::atoi(next());
        else if (a == "--time") fxStart = (float)std::atof(next());
        else if (a == "--low") low = true;
        else if (a == "--no-blur") fxo.blur = false;
        else if (a == "--no-lines") fxo.lines = false;
        else if (a == "--no-flames") fxo.flames = false;
        else if (a == "--nitro") fxo.forceNitro = std::atoi(next());
        else if (a == "--slip") fxo.forceSlip = std::atoi(next());
        else if (a == "--frames") shotFrames = std::max(1, std::atoi(next()));
        else if (a == "--fov") fov = (float)std::atof(next());
        else if (a == "--look")
        {
            // free camera at the origin: yaw (deg, 0 = -Z, 90 = -X / left), pitch (deg, up +)
            look = true;
            std::sscanf(next(), "%f,%f", &lookYaw, &lookPitch);
        }
        else
        {
            std::printf("usage: %s [--model NAME] [--skybox NAME] [--screenshot out.png] [--angle DEG]\n"
                        "          [--dist M] [--height M] [--size WxH] [--no-shadows] [--sky-only]\n"
                        "          [--frames N (with --screenshot)]\n"
                        "          [--fov DEG] [--look YAW,PITCH]\n"
                        "   hud: --hud [--hud-theme initiald|drifto_arcade] [--hud-scale S] [--night 0|1]\n"
                        "   effects: --fx [--time T] [--nitro 0|1] [--slip 0|1] [--no-blur] [--no-lines] [--no-flames] [--low]\n",
                        argv[0]);
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }

    if (!glfwInit())
    {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    if (!shot.empty()) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow *win = glfwCreateWindow(W, H, "gfx_demo", nullptr, nullptr);
    if (!win)
    {
        std::fprintf(stderr, "cannot create a GL 3.3 core window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(win);
    glfwSwapInterval(shot.empty() ? 1 : 0);

    int rc = 0;
    {
        gfx::Renderer r;
        if (!r.init(win, "assets"))
        {
            glfwTerminate();
            return 1;
        }
        r.setShadowsEnabled(shadows);
        gfx::HudGauges gauges;
        if (hudMode) gauges.init(r, hudTheme);
        if (low || !fxo.blur || !fxo.flames)
        {
            gfx::FxQuality q = r.fxQuality();
            if (!fxo.flames) q.flames = false;
            if (low) q.postFx = false;
            if (!fxo.blur) q.motionBlur = false;
            r.setFxQuality(q);
        }
        auto skies = r.availableSkyboxes();
        auto models = r.availableModels();
        std::printf("%zu models, %zu skyboxes\n", models.size(), skies.size());
        if (!r.setSkybox(skybox)) std::fprintf(stderr, "skybox '%s' not available\n", skybox.c_str());

        gfx::MeshHandle ground = r.createMesh(makeGround(600.0f, 120));
        gfx::ModelHandle ae86 = r.loadModel("ae86"), rx7 = r.loadModel("rx7");
        gfx::ModelHandle body = r.loadModel("ae86_body"), wheel = r.loadModel("ae86_wheel");
        gfx::ModelHandle centre = model.empty() ? 0 : r.loadModel(model);
        std::vector<gfx::ModelHandle> trees;
        for (const std::string &m : models)
            if (m.rfind("tree_", 0) == 0 && m.find('_', 11) == std::string::npos) trees.push_back(r.loadModel(m));

        auto drawHud = [&](float t) {
            if (!hudMode) return;
            gfx::TachState tach;
            gfx::NitroGaugeState ns;
            hudSim(t, tach, ns);
            Vec3 h = r.skyboxHorizonColor();
            bool night = nightMode >= 0 ? nightMode == 1 : (0.3f * h.x + 0.59f * h.y + 0.11f * h.z) < 0.25f;
            tach.night = ns.night = night;
            gauges.drawCluster(r, tach, ns, hudScale);
        };
        double t0 = glfwGetTime();
        int frame = 0;
        double tBench = 0;
        int modelIdx = -1;
        bool keyLatch = false;
        while (!glfwWindowShouldClose(win))
        {
            glfwPollEvents();
            if (glfwGetKey(win, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
            bool anyKey = false;
            for (int k = 0; k < 10 && !skies.empty(); ++k)
                if (glfwGetKey(win, GLFW_KEY_0 + k) == GLFW_PRESS)
                {
                    anyKey = true;
                    if (!keyLatch) r.setSkybox(skies[(k + 9) % 10 % skies.size()]);
                }
            if (glfwGetKey(win, GLFW_KEY_M) == GLFW_PRESS)
            {
                anyKey = true;
                if (!keyLatch && !models.empty())
                {
                    modelIdx = (modelIdx + 1) % (int)models.size();
                    model = models[modelIdx];
                    centre = r.loadModel(model);
                }
            }
            keyLatch = anyKey;

            if (fxMode)
            {
                float t = (fxStart >= 0 && !shot.empty()) ? fxStart + frame / 30.0f
                          : (float)(glfwGetTime() - t0) + std::max(0.0f, fxStart);
                FxState st = fxSim(t, fxo);
                Mat4 car = translate({st.x, 0, st.z}) * toMat4(quatYaw(st.yaw));
                Vec3 fwd = transformDir(car, {0, 0, -1});
                gfx::Camera cam;
                cam.position = Vec3{st.x * 0.6f, 2.0f, st.z} - fwd * 6.2f;
                cam.target = Vec3{st.x, 0.9f, st.z} + fwd * 3.0f;
                cam.fovY = gfx::speedFovY(st.speed01, st.nitro01);
                if (angleDeg >= 0) // orbit the player car instead of chasing it
                {
                    Vec3 c{st.x, 0.6f, st.z};
                    float a = radians(angleDeg) + st.yaw;
                    cam.position = c + Vec3{std::sin(a) * dist, camH, std::cos(a) * dist};
                    cam.target = c;
                    cam.fovY = radians(fov);
                }
                gfx::Lighting L;
                L.fogColor = r.skyboxHorizonColor();
                L.fogStart = 60.0f;
                L.fogEnd = 300.0f;
                r.beginFrame(cam, L);
                gfx::FrameFx fx;
                fx.speed01 = st.speed01;
                fx.nitro01 = st.nitro01;
                fx.slipstream01 = st.slip;
                fx.speedLines = fxo.lines ? 1.0f : 0.0f;
                fx.time = t;
                r.setFrameFx(fx);

                gfx::DrawParams gp;
                gp.outline = false;
                gp.castShadow = false;
                r.drawMesh(ground, translate({0, 0, std::floor(st.z / 8.0f) * 8.0f}), gp);
                // player car: camera-attached -> excluded from camera motion blur
                gfx::DrawParams pp;
                pp.motionBlur = false;
                r.drawModel(body, car, pp);
                const char *keys[4] = {"wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr"};
                for (int w = 0; w < 4; ++w)
                {
                    Vec3 p;
                    if (!r.modelPoint(body, keys[w], p)) continue;
                    r.drawModel(wheel, car * translate(p) * toMat4(quatYaw((w & 1) ? kPi : 0.0f)) *
                                           toMat4(quatAxisAngle({1, 0, 0}, ((w & 1) ? 1.0f : -1.0f) * st.wheelAngle)),
                                pp);
                }
                r.drawCarNitro(body, car, st.nitro01, t, st.ignition);
                // rival ahead (moves with the world -> normal motion blur is fine)
                Mat4 rxm = translate({st.x * 0.4f + 0.2f, 0, st.z - st.rxGap});
                r.drawModel(rx7, rxm);
                r.drawCarNitro(rx7, rxm, st.rxNitro, t, st.rxIgnition);
                // trees on a fixed world grid along the road
                int k0 = (int)std::floor(st.z / 14.0f);
                for (int k = k0 - 22; k <= k0 + 3; ++k)
                    for (int side = 0; side < 2; ++side)
                    {
                        float h = hash1((float)k * 2.0f + side);
                        float x = (side ? 1.0f : -1.0f) * (8.0f + 14.0f * h);
                        size_t ti = (size_t)(hash1(k * 7.0f + side * 3.0f) * trees.size()) % trees.size();
                        r.drawModel(trees[ti], translate({x, 0, k * 14.0f + 5.0f * h}) * toMat4(quatYaw(h * 6.0f)));
                    }
                char hud[256];
                std::snprintf(hud, sizeof hud, "gfx_demo --fx  t=%.2f  speed %.2f  nitro %.2f  slip %.2f  fov %.0f",
                              t, st.speed01, st.nitro01, st.slip, degrees(cam.fovY));
                r.drawRect(8, 8, 620, 26, {0, 0, 0}, 0.45f);
                r.drawText(16, 14, 1.5f, hud, {1, 1, 0.85f});
                // nitro gauge
                r.drawRect(16, 44, 204, 14, {0, 0, 0}, 0.5f);
                r.drawRect(18, 46, 200 * (st.nitro01 > 0 ? 1.0f - std::fmod(t, 7.0f) / 7.0f : 0.8f), 10,
                           {0.3f, 0.75f, 1.0f}, 0.9f);
                drawHud(t);
                r.endFrame();
            }
            else
            {
            float t = (float)(glfwGetTime() - t0);
            float ang = angleDeg >= 0 ? radians(angleDeg) : t * 0.25f + 0.6f;
            Vec3 target{0, 0.8f, 0};
            gfx::Camera cam;
            cam.position = target + Vec3{std::sin(ang) * dist, camH, std::cos(ang) * dist};
            cam.target = target;
            cam.fovY = radians(fov);
            if (look)
            {
                cam.position = {0, 1.6f, 0};
                Quat q = quatYaw(radians(lookYaw)) * quatAxisAngle({1, 0, 0}, radians(lookPitch));
                cam.target = cam.position + forwardOf(q);
                if (std::fabs(lookPitch) > 89.0f) cam.up = rotate(quatYaw(radians(lookYaw)), {0, 0, -1});
            }
            gfx::Lighting L;
            L.fogColor = r.skyboxHorizonColor();
            float lum = 0.3f * L.fogColor.x + 0.59f * L.fogColor.y + 0.11f * L.fogColor.z;
            if (lum < 0.25f) // night sky: moonlight
            {
                L.sunColor = {0.45f, 0.5f, 0.75f};
                L.ambient = {0.14f, 0.16f, 0.26f};
            }
            L.fogStart = 40.0f;
            L.fogEnd = 220.0f;

            r.beginFrame(cam, L);
            gfx::DrawParams ground_p;
            ground_p.outline = false;
            ground_p.castShadow = false; // nothing below the ground to shadow
            if (!skyOnly) r.drawMesh(ground, Mat4(), ground_p);
            if (skyOnly)
                ;
            else if (centre)
                r.drawModel(centre, Mat4());
            else
            {
                r.drawModel(ae86, translate({-1.6f, 0, 0}));
                r.drawModel(rx7, translate({1.6f, 0, 0}));
                // AE86 assembled from parts, behind the others, wheels steered
                Mat4 car = translate({0, 0, 6.0f});
                r.drawModel(body, car);
                const char *keys[4] = {"wheel_fl", "wheel_fr", "wheel_rl", "wheel_rr"};
                for (int w = 0; w < 4; ++w)
                {
                    Vec3 p;
                    if (!r.modelPoint(body, keys[w], p)) continue;
                    float steer = w < 2 ? 0.35f : 0.0f;
                    float yaw = steer + ((w & 1) ? kPi : 0.0f);
                    r.drawModel(wheel, car * translate(p) * toMat4(quatYaw(yaw)) *
                                           toMat4(quatAxisAngle({1, 0, 0}, -t * 6.0f)));
                }
                // smoke puffs behind the AE86's rear tyres
                for (int k = 0; k < 10; ++k)
                {
                    float f = std::fmod(t * 0.6f + k * 0.1f, 1.0f);
                    Vec3 base{-1.6f + ((k & 1) ? 0.7f : -0.7f), 0.35f + f * 1.2f, 1.3f + f * 4.0f};
                    r.drawBillboard(base, 0.8f + f * 2.0f, {0.92f, 0.92f, 0.95f}, 0.7f * (1.0f - f));
                }
            }
            for (size_t i = 0; !skyOnly && i < trees.size() * 2; ++i)
            {
                bool outer = i >= trees.size();
                float rad = outer ? 70.0f : 22.0f;
                float a = (float)i / (float)trees.size() * 2 * kPi + (outer ? 0.13f : 0.0f);
                r.drawModel(trees[i % trees.size()],
                            translate({std::sin(a) * rad, 0, std::cos(a) * rad}) * toMat4(quatYaw(a * 3.0f)));
            }
            char hud[256];
            std::snprintf(hud, sizeof hud, "gfx_demo  sky: %s  model: %s  %dx%d", skybox.c_str(),
                          model.empty() ? "(scene)" : model.c_str(), r.framebufferWidth(), r.framebufferHeight());
            r.drawRect(8, 8, 520, 26, {0, 0, 0}, 0.45f);
            r.drawText(16, 14, 1.5f, hud, {1, 1, 0.85f});
            drawHud((fxStart >= 0 && !shot.empty()) ? fxStart + frame / 30.0f : t);
            r.endFrame();
            }
            glfwSwapBuffers(win);
            ++frame;
            if (frame == 5) tBench = glfwGetTime();
            if (!shot.empty() && frame >= shotFrames)
            {
                if (frame > 5)
                {
                    double secs = glfwGetTime() - tBench;
                    std::printf("%.1f ms/frame (%.1f fps, %d frames)\n", secs * 1000.0 / (frame - 5),
                                (frame - 5) / secs, frame - 5);
                }
                rc = r.saveScreenshot(shot) ? 0 : 1;
                std::printf("%s %s\n", rc == 0 ? "wrote" : "FAILED", shot.c_str());
                break;
            }
        }
        r.shutdown();
    }
    glfwDestroyWindow(win);
    glfwTerminate();
    return rc;
}
