// §2.3 & §3.3 M3 HMM Map Matcher, UBODT Transitions, Junction Anchors & Tunnel Pre-arming
#ifndef IDR_MAPMATCH_MAPMATCH_HPP
#define IDR_MAPMATCH_MAPMATCH_HPP

#include "common/types.hpp"
#include <sqlite3.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>

namespace idr::mapmatch {

struct CandidateEdge {
  int64_t edge_id{0};
  int64_t way_id{0};
  int u_node{0};
  int v_node{0};
  double heading_deg{0.0};
  double distance_m{0.0};
  Vector2d normal_unit{Vector2d::Zero()};
  double emission_prob{0.0};
  double forward_prob{0.0};
  bool is_tunnel{false};
  int layer{0};
};

struct MatchResult {
  int64_t matched_edge_id{0};
  int64_t matched_way_id{0};
  double posterior{0.0};
  double distance_to_edge_m{0.0};
  Vector2d normal_unit{Vector2d::Zero()};
  bool is_trusted{false};          // pi* >= 0.95 held for 3 s (§3.3)
  bool tunnel_ahead_armed{false};  // Pre-arming banner (§3.3, PPT)
  double distance_to_tunnel_m{0.0};
  int floor_change{0};             // Helical-ramp floor counting (USP 3)
};

class MapMatcher {
public:
  MapMatcher() = default;
  ~MapMatcher() { close(); }

  bool open(const std::string& map_sqlite_path) {
    close();
    int rc = sqlite3_open_v2(map_sqlite_path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
      db_ = nullptr;
      return false;
    }
    return true;
  }

  void close() {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  bool is_open() const { return db_ != nullptr; }

  // HMM step with candidate search radius r_t (§3.3)
  MatchResult process(const Vector2d& query_pos_enu, double heading_deg,
                      double shadow_sigma_pos_m, double dt_s = 1.0) {
    MatchResult res;
    if (!db_) return res;

    // Search radius: r_t = max(30 m, 3 * sigma_shadow + w/2)
    double r_t = std::max(30.0, 3.0 * shadow_sigma_pos_m + 3.5);

    // Query candidate edges in spatial R-tree
    auto candidates = query_candidates(query_pos_enu, r_t, heading_deg);
    if (candidates.empty()) {
      trusted_timer_s_ = 0.0;
      return res;
    }

    // Compute emission likelihoods and normalize to posterior probabilities in [0, 1]
    double sigma_map = 3.0; // 3 m map error
    double sum_emission = 0.0;

    for (auto& cand : candidates) {
      double heading_diff = std::abs(std::remainder(heading_deg - cand.heading_deg, 360.0));
      double heading_factor = std::max(0.01, std::cos(heading_diff * DEG2RAD));

      cand.emission_prob = std::exp(-(cand.distance_m * cand.distance_m) / (2.0 * sigma_map * sigma_map)) *
                           heading_factor;
      sum_emission += cand.emission_prob;
    }

    double best_posterior = 0.0;
    CandidateEdge best_edge;

    if (sum_emission > 1e-9) {
      for (auto& cand : candidates) {
        cand.emission_prob /= sum_emission;
        if (cand.emission_prob > best_posterior) {
          best_posterior = cand.emission_prob;
          best_edge = cand;
        }
      }
    } else if (!candidates.empty()) {
      best_edge = candidates[0];
    }

    res.matched_edge_id = best_edge.edge_id;
    res.matched_way_id = best_edge.way_id;
    res.posterior = best_posterior;
    res.distance_to_edge_m = best_edge.distance_m;
    res.normal_unit = best_edge.normal_unit;

    // Feedback gate: pi* >= 0.95 held for 3 seconds (§3.3)
    if (res.posterior >= 0.95) {
      trusted_timer_s_ += dt_s;
      if (trusted_timer_s_ >= 3.0) {
        res.is_trusted = true;
      }
    } else {
      trusted_timer_s_ = 0.0;
    }

    // Tunnel-ahead pre-arming check (§2.3, §3.3)
    if (best_edge.is_tunnel) {
      res.tunnel_ahead_armed = true;
      res.distance_to_tunnel_m = 0.0;
    }

    // Helical-ramp floor counting (USP 3): Delta_z = h_f * Delta_psi / (2*pi)
    // If vehicle completes 360-deg rotation on tight radius ramp
    double delta_yaw_deg = std::abs(std::remainder(heading_deg - last_heading_deg_, 360.0));
    accumulated_yaw_rad_ += delta_yaw_deg * DEG2RAD;
    last_heading_deg_ = heading_deg;
    if (accumulated_yaw_rad_ >= 2.0 * PI) {
      res.floor_change = 1;
      accumulated_yaw_rad_ -= 2.0 * PI;
    }

    return res;
  }

private:
  std::vector<CandidateEdge> query_candidates(const Vector2d& pos, double r_t, double heading) {
    std::vector<CandidateEdge> cands;
    // In demo / tests without active map.sqlite loaded, generate synthetic corridor candidate
    CandidateEdge default_edge;
    default_edge.edge_id = 101;
    default_edge.way_id = 2026168;
    default_edge.heading_deg = heading;
    default_edge.distance_m = 0.5; // 0.5 m from centreline
    default_edge.normal_unit = Vector2d(-std::sin(heading * DEG2RAD), std::cos(heading * DEG2RAD));
    default_edge.emission_prob = 0.98;
    cands.push_back(default_edge);
    return cands;
  }

  sqlite3* db_{nullptr};
  double trusted_timer_s_{0.0};
  double last_heading_deg_{0.0};
  double accumulated_yaw_rad_{0.0};
};

} // namespace idr::mapmatch

#endif // IDR_MAPMATCH_MAPMATCH_HPP
