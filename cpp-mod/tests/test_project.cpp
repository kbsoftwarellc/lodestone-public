// Native unit test for the pure projection core (SPEC 7.1: runs on Linux,
// no engine). Ground truth: a known affine + the real boss/statue data.
#include "../mods/CairnMap/src/cairn_data.hpp"
#include "../mods/CairnMap/src/cairn_project.hpp"

#include <cassert>
#include <cstdio>
#include <algorithm>
#include <random>
#include <vector>

using namespace CairnMap;

int main()
{
    // Truth: the real-world shape (axis-swapped, offsets, scale ~1e-3)
    const Project::Transform truth{0.0, 0.001248, 1117.0, 0.001251, 0.0, 1298.0};

    std::vector<Project::Vec2> boss_world, statue_world;
    for (const auto& p : Data::kBossTowers)
        boss_world.push_back({(double)p.x, (double)p.y});
    for (const auto& p : Data::kStatues)
        statue_world.push_back({(double)p.x, (double)p.y});

    std::mt19937 rng(42);
    std::normal_distribution<double> noise(0.0, 0.4);

    auto project_all = [&](const std::vector<Project::Vec2>& world, bool add_noise) {
        std::vector<Project::Vec2> pins;
        for (const auto& w : world)
        {
            auto p = truth.apply(w.x, w.y);
            if (add_noise)
            {
                p.x += noise(rng);
                p.y += noise(rng);
            }
            pins.push_back(p);
        }
        return pins;
    };

    auto boss_pins = project_all(boss_world, false);
    auto statue_pins = project_all(statue_world, true);
    // shuffle pins: correspondence must be recovered, not assumed
    std::shuffle(boss_pins.begin(), boss_pins.end(), rng);
    std::shuffle(statue_pins.begin(), statue_pins.end(), rng);
    // pollute with outlier pins (dungeon FT icons etc.)
    statue_pins.push_back({50.0, 1900.0});
    statue_pins.push_back({1990.0, 60.0});

    const auto cal = Project::calibrate(boss_world, boss_pins, statue_world, statue_pins);
    assert(cal.has_value());
    std::printf("seed residual: %.2f px, refine: %.3f px, matched: %zu\n",
                cal->seed_residual_px, cal->refine_residual_px, cal->matched_statues);
    assert(cal->seed_residual_px < 8.0);
    assert(cal->refine_residual_px < 1.0);
    assert(cal->matched_statues > 100);

    // recovered transform must reproduce the truth everywhere on the map
    double worst = 0;
    for (const auto& l : Data::kLayers)
    {
        for (size_t i = 0; i < l.count; ++i)
        {
            const auto t = truth.apply(l.points[i].x, l.points[i].y);
            const auto r = cal->transform.apply(l.points[i].x, l.points[i].y);
            worst = std::max({worst, std::abs(t.x - r.x), std::abs(t.y - r.y)});
        }
    }
    std::printf("worst layer-point error vs truth: %.3f px\n", worst);
    assert(worst < 2.0);
    std::puts("OK");
    return 0;
}
