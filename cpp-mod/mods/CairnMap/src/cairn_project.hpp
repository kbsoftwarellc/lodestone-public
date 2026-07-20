// CairnMap — world -> map-canvas projection (pure, no engine deps).
// SPEC 2.3: exact seed from the 8 boss towers (farthest-pair correspondence,
// 4 orientation hypotheses), refined by one matched least-squares pass over
// the fast-travel statue pins. Field-proven at 0 px (seed) / 0.3 px (refine).
// ⚠ Centered least squares only: raw 1e6-scale world coords cancel
//   catastrophically in naive normal equations (proven live 2026-07-12).
#pragma once
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace CairnMap::Project
{
    struct Vec2
    {
        double x{}, y{};
    };

    // canvas = {ax*wx + bx*wy + cx, ay*wx + by*wy + cy}
    struct Transform
    {
        double ax{}, bx{}, cx{};
        double ay{}, by{}, cy{};

        [[nodiscard]] auto apply(double wx, double wy) const -> Vec2
        {
            return {ax * wx + bx * wy + cx, ay * wx + by * wy + cy};
        }
    };

    struct Anchor
    {
        double wx, wy;   // world
        double px, py;   // canvas
    };

    namespace detail
    {
        struct Fit1D
        {
            double p, q, r;   // s = p*wx + q*wy + r
        };

        // Centered least squares for s = p*wx + q*wy + r.
        inline auto fit_axis(const std::vector<Anchor>& m, bool use_px) -> std::optional<Fit1D>
        {
            const auto n = static_cast<double>(m.size());
            if (m.size() < 5)
            {
                return std::nullopt;
            }
            double mx{}, my{}, ms{};
            for (const auto& a : m)
            {
                mx += a.wx;
                my += a.wy;
                ms += use_px ? a.px : a.py;
            }
            mx /= n;
            my /= n;
            ms /= n;
            double cxx{}, cxy{}, cyy{}, cxs{}, cys{};
            for (const auto& a : m)
            {
                const double dx = a.wx - mx;
                const double dy = a.wy - my;
                const double ds = (use_px ? a.px : a.py) - ms;
                cxx += dx * dx;
                cxy += dx * dy;
                cyy += dy * dy;
                cxs += dx * ds;
                cys += dy * ds;
            }
            const double det = cxx * cyy - cxy * cxy;
            if (std::abs(det) < 1e-6)
            {
                return std::nullopt;
            }
            const double p = (cxs * cyy - cys * cxy) / det;
            const double q = (cys * cxx - cxs * cxy) / det;
            return Fit1D{p, q, ms - p * mx - q * my};
        }

        inline auto fit_affine(const std::vector<Anchor>& m) -> std::optional<Transform>
        {
            const auto fx = fit_axis(m, true);
            const auto fy = fit_axis(m, false);
            if (!fx || !fy)
            {
                return std::nullopt;
            }
            return Transform{fx->p, fx->q, fx->r, fy->p, fy->q, fy->r};
        }

        template <typename Range>
        inline auto farthest_pair(const Range& pts) -> std::pair<size_t, size_t>
        {
            size_t bi = 0, bj = 1;
            double bd = -1.0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                for (size_t j = i + 1; j < pts.size(); ++j)
                {
                    const double dx = pts[i].x - pts[j].x;
                    const double dy = pts[i].y - pts[j].y;
                    const double d = dx * dx + dy * dy;
                    if (d > bd)
                    {
                        bd = d;
                        bi = i;
                        bj = j;
                    }
                }
            }
            return {bi, bj};
        }
    } // namespace detail

    struct Calibration
    {
        Transform transform;
        double seed_residual_px;     // total over boss towers
        double refine_residual_px;   // mean over matched statues
        size_t matched_statues;
    };

    // world positions of bosses/statues come from cairn_data.hpp; pins are the
    // game's own icon slot positions read at runtime.
    inline auto calibrate(const std::vector<Vec2>& boss_world,
                          const std::vector<Vec2>& boss_pins,
                          const std::vector<Vec2>& statue_world,
                          const std::vector<Vec2>& statue_pins) -> std::optional<Calibration>
    {
        if (boss_world.size() < 4 || boss_pins.size() < 4)
        {
            return std::nullopt;
        }
        const auto [wa, wb] = detail::farthest_pair(boss_world);
        const auto [pa, pb] = detail::farthest_pair(boss_pins);

        // ⚠ SPEC 2.3: blind matching collapses into local optima; the exact
        // boss correspondence (farthest pair x 4 orientation hypotheses) is
        // what makes this deterministic.
        std::optional<Transform> best;
        double best_err = 1e18;
        for (const bool axis_swap : {true, false})     // canvasX from worldY (true) or worldX
        {
            for (const bool flip : {false, true})      // A->A or A->B
            {
                const Vec2& qa = flip ? boss_pins[pb] : boss_pins[pa];
                const Vec2& qb = flip ? boss_pins[pa] : boss_pins[pb];
                const double wua = axis_swap ? boss_world[wa].y : boss_world[wa].x;
                const double wub = axis_swap ? boss_world[wb].y : boss_world[wb].x;
                const double wva = axis_swap ? boss_world[wa].x : boss_world[wa].y;
                const double wvb = axis_swap ? boss_world[wb].x : boss_world[wb].y;
                if (std::abs(wub - wua) < 1.0 || std::abs(wvb - wva) < 1.0)
                {
                    continue;
                }
                const double a = (qb.x - qa.x) / (wub - wua);
                const double b = qa.x - a * wua;
                const double c = (qb.y - qa.y) / (wvb - wva);
                const double d = qa.y - c * wva;
                const Transform t = axis_swap ? Transform{0, a, b, c, 0, d}
                                              : Transform{a, 0, b, 0, c, d};
                double err = 0;
                for (const auto& w : boss_world)
                {
                    const auto p = t.apply(w.x, w.y);
                    double bd = 1e18;
                    for (const auto& q : boss_pins)
                    {
                        const double dx = q.x - p.x;
                        const double dy = q.y - p.y;
                        bd = std::min(bd, dx * dx + dy * dy);
                    }
                    err += std::sqrt(bd);
                }
                if (err < best_err)
                {
                    best_err = err;
                    best = t;
                }
            }
        }
        if (!best)
        {
            return std::nullopt;
        }

        // Refine on statues: nearest-pin matching under the exact seed, then
        // one centered least-squares pass.
        std::vector<Anchor> matched;
        for (const auto& w : statue_world)
        {
            const auto p = best->apply(w.x, w.y);
            double bd = 60.0 * 60.0;
            const Vec2* hit = nullptr;
            for (const auto& q : statue_pins)
            {
                const double dx = q.x - p.x;
                const double dy = q.y - p.y;
                const double d = dx * dx + dy * dy;
                if (d < bd)
                {
                    bd = d;
                    hit = &q;
                }
            }
            if (hit)
            {
                matched.push_back({w.x, w.y, hit->x, hit->y});
            }
        }
        Transform final_t = *best;
        if (matched.size() >= 15)
        {
            if (const auto refined = detail::fit_affine(matched))
            {
                final_t = *refined;
            }
        }
        double refine_err = 0;
        for (const auto& a : matched)
        {
            const auto p = final_t.apply(a.wx, a.wy);
            refine_err += std::abs(p.x - a.px) + std::abs(p.y - a.py);
        }
        const double mean_err =
            matched.empty() ? 0.0 : refine_err / (2.0 * static_cast<double>(matched.size()));
        return Calibration{final_t, best_err, mean_err, matched.size()};
    }
} // namespace CairnMap::Project
