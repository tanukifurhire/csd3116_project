// world_view.h -- turns game state (track, cars, effects) into renderer
// calls. Owns every GPU resource the game creates: streamed section meshes,
// skid-mark meshes, fallback car/tree meshes. Missing models degrade to
// procedural ones, so the game runs even without `make assets`.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "game/include/multiplayer_session.h"
#include "game/include/skid_marks.h"
#include "game/include/track.h"
#include "game/include/track_mesh.h"
#include "gfx/include/renderer.h"

namespace client {

// The four paints (CarView::look): white, gold, red, blue -- one per room
// slot, so cars stay distinguishable whichever model (CarView::model, the
// player's AE86 / RX-7 pick) they are. HUD swatches use lookColor().
dm::Vec3 lookColor(int look);
const char *lookName(int look);

class WorldView
{
public:
    explicit WorldView(gfx::Renderer &renderer);
    ~WorldView();

    // After renderer.init(): resolve models / build fallbacks.
    void initAssets();
    void shutdown();

    // Sections kept alive around the viewer (the original's forward/rear
    // buffer, a bit wider because the switchback below stays visible).
    // (distances, because sections now range from a few metres to 70 m)
    // (kept 120 m behind: at 40 m the road and terrain beside the camera
    // vanished on switchbacks and when the camera swung round in a drift)
    float viewBehind = 120.0f; // metres of road kept behind the last car
    float viewAhead = 320.0f;  // ...and ahead of the leader / camera car
    float treeAhead = 200.0f;  // trees only within this distance of the camera car
    int maxSections = 64;      // cap on how many sections are alive at once
    float valleyDepth = 38.0f; // valley floor below the viewer's car
    bool treeOutlines = true;

    // Call when the track (seed) changes: drops all streamed geometry/effects.
    void reset(const game::Track &track);
    // Create/destroy section meshes around `currentSection` (rate-limited).
    // fieldFirst/fieldLast: sections of the last and leading live car -- the
    // whole field keeps road under it, not just the camera car.
    void streamSections(const game::Track &track, uint32_t currentSection, uint32_t fieldFirst, uint32_t fieldLast);
    // Once per simulation tick: skid marks + smoke emission.
    void onSimTick(const game::Track &track, const std::vector<game::CarView> &cars);
    // Queue all world draws for this frame (between beginFrame/endFrame).
    // cameraTargetId: the car the camera follows (valley floor and the tree
    // radius are centred on it).
    void draw(const std::vector<game::CarView> &cars, float frameDt, float time, int32_t cameraTargetId);

    uint32_t trackSeed() const { return seed_; }
    bool hasModel(const std::string &name) const { return available_.count(name) != 0; }

private:
    struct TreeInstance
    {
        gfx::ModelHandle model = 0; // 0 => fallback mesh
        int fallback = 0;
        dm::Mat4 xform;
    };
    struct SectionGfx
    {
        gfx::MeshHandle road = 0, terrain = 0, props = 0;
        std::vector<TreeInstance> trees;
    };
    struct CarModel
    {
        gfx::ModelHandle full = 0, body = 0, wheel = 0;
        dm::Vec3 wheelPos[4];
        float wheelRadius = 0.3f;
        float scale = 1.0f;
    };
    struct CarAnim
    {
        float spin = 0, steer = 0;
        float nitro = 0, ignition = 0; // smoothed nitro flame, ignition burst
        bool wasBurning = false;
        game::SkidMarks marks;
        gfx::MeshHandle marksMesh = 0;
    };
    struct Particle
    {
        dm::Vec3 pos, vel;
        float age, life, size, alpha;
    };

    void destroySection(SectionGfx &s);
    void loadCarModel(const std::string &name, CarModel &out);
    const CarModel &carModelFor(int model) const;
    void drawCar(const game::CarView &car, CarAnim &anim, float dt, float time);
    float rand01();

    gfx::Renderer &r_;
    std::unordered_set<std::string> available_;
    std::string treeSuffix_;
    uint32_t seed_ = 0;
    bool haveTrack_ = false;
    std::map<uint32_t, SectionGfx> sections_;
    CarModel ae86_, rx7_;
    gfx::MeshHandle boxCar_ = 0;
    gfx::MeshHandle valley_ = 0; // far-below ground disc so gaps never show sky
    gfx::MeshHandle fallbackTrees_[3] = {0, 0, 0};
    std::map<int32_t, CarAnim> anims_;
    std::vector<Particle> particles_;
    uint32_t rng_ = 0x1234567u;
};

} // namespace client
