// §3.0 Top-Level Navigation Engine Implementation
#include "engine.hpp"
#include <iostream>
#include <cstring>
#include <sstream>

namespace idr::engine {

NavigationEngine::NavigationEngine(const idr_config& config)
    : config_(config),
      frontend_(config.imu_rate_hz > 0.0 ? config.imu_rate_hz : 10.0) {
  std::memset(&current_state_, 0, sizeof(current_state_));
  current_state_.mode = IDR_MODE_GNSS_OK;
  current_state_.vehicle_mode = IDR_VEHICLE_CAR;
  current_state_.hpl_m = config.alert_limit_m > 0 ? config.alert_limit_m : 20.0;
}

bool NavigationEngine::load_models(const std::string& speed_onnx, const std::string& adapter_onnx) {
  std::lock_guard<std::mutex> lock(mutex_);
  bool s_ok = speed_tcn_.load_model(speed_onnx);
  bool a_ok = noise_adapter_.load_model(adapter_onnx);
  return s_ok && a_ok;
}

bool NavigationEngine::load_map(const std::string& map_sqlite, const std::string& /*user_sqlite*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  return map_matcher_.open(map_sqlite);
}

void NavigationEngine::push_imu(const idr_imu& imu) {
  std::lock_guard<std::mutex> lock(mutex_);
  ImuSample s;
  s.t_ns = imu.t_ns;
  s.accel = Vector3d(imu.ax, imu.ay, imu.az);
  s.gyro = Vector3d(imu.gx, imu.gy, imu.gz);

  // Gyro-less phone fallback (§5.5)
  if (!config_.has_gyroscope) {
    s.gyro = Vector3d::Zero();
  }

  double dt_s = 0.1;
  if (last_step_ns_ > 0 && imu.t_ns > last_step_ns_) {
    dt_s = (imu.t_ns - last_step_ns_) * 1e-9;
    dt_s = std::clamp(dt_s, 0.001, 0.5);
  }
  last_step_ns_ = imu.t_ns;

  // High-rate state propagation
  main_filter_.predict(s.accel, s.gyro, dt_s);
  shadow_filter_.predict(s.accel, s.gyro, dt_s);

  imu_buffer_.push_back(s);
  if (imu_buffer_.size() > 50) {
    imu_buffer_.pop_front();
  }
}

void NavigationEngine::push_mag(const idr_mag& mag) {
  std::lock_guard<std::mutex> lock(mutex_);
  Vector3d m_body(mag.mx, mag.my, mag.mz);
  geo::MagneticFieldEstimate dummy_igrf;
  dummy_igrf.valid = true;
  dummy_igrf.total_intensity_ut = 45.0;
  dummy_igrf.declination_deg = 0.5;
  dummy_igrf.inclination_deg = 35.0;

  double prior_heading = 0.0;
  if (alignment_.evaluate_magnetometer_prior(m_body, dummy_igrf, prior_heading)) {
    // Start-up heading prior accepted (§3.1)
  }
}

void NavigationEngine::push_baro(const idr_baro& /*baro*/) {
  // Barometer update reserved for flyover separation & parking floor
}

void NavigationEngine::push_fix(const idr_fix& fix) {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_fix_.t_ns = fix.t_ns;
  latest_fix_.utc_ms = fix.utc_ms;
  latest_fix_.lat_deg = fix.lat_deg;
  latest_fix_.lon_deg = fix.lon_deg;
  latest_fix_.alt_m = fix.alt_m;
  latest_fix_.speed_mps = fix.speed_mps;
  latest_fix_.bearing_deg = fix.bearing_deg;
  latest_fix_.r68_m = fix.r68_m;
  latest_fix_.num_sats = fix.num_sats;
  latest_fix_.valid = (fix.valid != 0);
  has_new_fix_ = true;
}

void NavigationEngine::push_gnss_status(const idr_sat* sats, int n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (n <= 0 || !sats) return;
  std::vector<double> cn0s;
  cn0s.reserve(n);
  for (int i = 0; i < n; ++i) {
    if (sats[i].cn0_dbhz > 0.0) {
      cn0s.push_back(sats[i].cn0_dbhz);
    }
  }
  if (!cn0s.empty()) {
    std::sort(cn0s.begin(), cn0s.end());
    median_cn0_dbhz_ = cn0s[cn0s.size() / 2];
  }
}

void NavigationEngine::push_agc(const idr_agc* agc, int n) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (n > 0 && agc) {
    agc_change_db_ = agc[0].agc_db;
  }
}

void NavigationEngine::push_wheel_speed(double speed_mps) {
  std::lock_guard<std::mutex> lock(mutex_);
  wheel_speed_mps_ = speed_mps;
  has_wheel_speed_ = true;
}

int NavigationEngine::step(int64_t now_ns) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (last_10hz_ns_ == 0) {
    last_10hz_ns_ = now_ns;
    return 0;
  }

  // 10 Hz cycle execution (every 100 ms)
  if ((now_ns - last_10hz_ns_) >= 95'000'000) {
    run_10hz_cycle(now_ns);
    last_10hz_ns_ = now_ns;
  }
  return 0;
}

void NavigationEngine::run_10hz_cycle(int64_t now_ns) {
  if (imu_buffer_.empty()) return;

  ImuSample latest_imu = imu_buffer_.back();
  auto filtered = frontend_.process(latest_imu);

  // Vehicle frame specific force and angular velocity through mount rotation R_bv
  Matrix3d R_bv = alignment_.R_bv();
  main_filter_.mutable_state().R_bv = R_bv;
  shadow_filter_.mutable_state().R_bv = R_bv;
  Vector3d f_v = R_bv.transpose() * filtered.accel_filtered;
  Vector3d omega_v = R_bv.transpose() * filtered.gyro_filtered;

  // M1 Up-tracker update
  bool is_stopped = glrt_.is_stationary();
  alignment_.update_up_tracker(filtered.accel_filtered, 0.1, is_stopped);

  // M2 Speed TCN inference
  speed_tcn_.push_features(f_v, omega_v);
  double current_filt_speed = main_filter_.state().v.norm();
  auto speed_out = speed_tcn_.predict(current_filt_speed);

  bool is_ins_only    = (config_.baseline_mode == IDR_BASELINE_INS_ONLY);
  bool is_nhc_only    = (config_.baseline_mode == IDR_BASELINE_NHC_ONLY);
  bool is_no_adapter  = (config_.baseline_mode == IDR_BASELINE_NO_ADAPTER);
  bool is_no_map      = (config_.baseline_mode == IDR_BASELINE_NO_MAP);
  bool is_whonet      = (config_.baseline_mode == IDR_BASELINE_WHONET);

  // WhONet baseline: override speed with physical wheel speed measurement
  if (is_whonet && has_wheel_speed_) {
    speed_out.mu_mps = wheel_speed_mps_;
    speed_out.var_mps2 = 0.05 * 0.05;
    speed_out.sigma_mps = 0.05;
  }

  // M2 GLRT Stop Detector update (evaluated in vehicle frame for robust reverse detection)
  bool now_stationary = glrt_.process(f_v, omega_v,
                                      speed_out.mu_mps, speed_out.sigma_mps, 0.1);

  // Stop updates: ZUPT, ZARU and Gravity Leveling (§3.1, §3.2)
  if (!is_ins_only && now_stationary) {
    main_filter_.update_zupt(0.01);
    shadow_filter_.update_zupt(0.01);
    main_filter_.update_zaru(filtered.gyro_filtered, 0.005);
    shadow_filter_.update_zaru(filtered.gyro_filtered, 0.005);
    main_filter_.update_gravity_level(filtered.accel_filtered, 0.02);
    shadow_filter_.update_gravity_level(filtered.accel_filtered, 0.02);
  } else if (!is_ins_only && std::abs(filtered.accel_filtered.norm() - STANDARD_GRAVITY) < 0.25) {
    // Madgwick-style continuous leveling when acceleration is primarily gravity (§3.1)
    main_filter_.update_gravity_level(filtered.accel_filtered, 0.05);
    shadow_filter_.update_gravity_level(filtered.accel_filtered, 0.05);
  }

  // M4 Noise Adapter inference
  noise_adapter_.push_features(f_v, omega_v, filtered.env_accel_z, filtered.env_accel_y,
                               filtered.env_gyro_x, speed_out.mu_mps);
  auto noise_scales = noise_adapter_.predict();
  if (is_no_adapter) {
    noise_scales.alpha_nhc_lat = 1.0;
    noise_scales.alpha_nhc_up = 1.0;
    noise_scales.alpha_speed = 1.0;
  }

  // M4 Filter updates
  // NHC update (lateral and vertical constraints)
  double a_lat = f_v(1);
  bool slide = vehicle_classifier_.is_sliding();
  bool shock = filtered.shock_flag;
  double nhc_nis = 0.0;
  if (!is_ins_only) {
    nhc_nis = main_filter_.update_nhc(noise_scales.alpha_nhc_lat, noise_scales.alpha_nhc_up,
                                      a_lat, slide || shock);
    shadow_filter_.update_nhc(noise_scales.alpha_nhc_lat, noise_scales.alpha_nhc_up,
                              a_lat, slide || shock);
  }

  // Speed update (Speed TCN or WhONet)
  if (!is_ins_only && !is_nhc_only && !now_stationary) {
    main_filter_.update_speed(speed_out.mu_mps, speed_out.var_mps2, noise_scales.alpha_speed, shock);
    shadow_filter_.update_speed(speed_out.mu_mps, speed_out.var_mps2, noise_scales.alpha_speed, shock);
  }

  // M1 Slip check
  alignment_.check_slip(nhc_nis, now_stationary, 0.1);
  alignment_.update_axes_from_motion(filtered.accel_filtered, filtered.gyro_filtered, speed_out.mu_mps);

  // Vehicle mode SPRT and slide gate update
  vehicle_classifier_.update_sprt(a_lat, omega_v(0), omega_v(2));
  vehicle_classifier_.update_slide_gate(omega_v(2) * speed_out.mu_mps - a_lat, 0.15, 0.1);

  // GNSS Fix processing & Deficit Handler
  bool fix_attempted = false;
  bool fix_accepted = false;
  double pos_nis = 0.0;
  double vel_res_mps = 0.0;

  if (has_new_fix_) {
    has_new_fix_ = false;
    fix_attempted = true;
    if (integrity_monitor_.pre_check_fix(latest_fix_, median_cn0_dbhz_, now_ns)) {
      if (!tangent_plane_.is_initialized()) {
        tangent_plane_.reset(latest_fix_.lat_deg, latest_fix_.lon_deg, latest_fix_.alt_m);

        // Leveling from gravity vector: up_b = accel_filtered normalized
        Vector3d up_b = filtered.accel_filtered.normalized();
        Vector3d up_w(0.0, 0.0, 1.0);
        Vector3d v_rot = up_b.cross(up_w);
        double c_th = up_b.dot(up_w);
        Matrix3d R_level = Matrix3d::Identity();
        if (v_rot.norm() > 1e-5 && c_th > -0.999) {
          Matrix3d v_skew = lie::skew(v_rot);
          R_level = Matrix3d::Identity() + v_skew + v_skew * v_skew * (1.0 / (1.0 + c_th));
        }

        // Heading from GNSS course if available
        double bearing = (latest_fix_.bearing_deg > 0.0) ? latest_fix_.bearing_deg : 0.0;
        double psi_enu = (90.0 - bearing) * DEG2RAD;
        Matrix3d R_yaw = Matrix3d::Identity();
        R_yaw(0,0) = std::cos(psi_enu);  R_yaw(0,1) = -std::sin(psi_enu);
        R_yaw(1,0) = std::sin(psi_enu);  R_yaw(1,1) =  std::cos(psi_enu);

        Matrix3d R_init = R_yaw * R_level;
        main_filter_.mutable_state().R = R_init;
        shadow_filter_.mutable_state().R = R_init;

        if (latest_fix_.speed_mps > 0.5) {
          Vector3d v_init(latest_fix_.speed_mps * std::cos(psi_enu),
                          latest_fix_.speed_mps * std::sin(psi_enu), 0.0);
          main_filter_.mutable_state().v = v_init;
          shadow_filter_.mutable_state().v = v_init;
        }
      }

      Vector3d fix_enu = tangent_plane_.forward(latest_fix_.lat_deg, latest_fix_.lon_deg, latest_fix_.alt_m);
      pos_nis = main_filter_.update_gnss_position(fix_enu.head<2>(), latest_fix_.r68_m, 25.0);
      shadow_filter_.update_gnss_position(fix_enu.head<2>(), latest_fix_.r68_m, 25.0);

      if (pos_nis <= 75.0) {
        fix_accepted = true;
        if (latest_fix_.speed_mps > 0.5) {
          double rad = (90.0 - latest_fix_.bearing_deg) * DEG2RAD;
          Vector2d vel_enu(latest_fix_.speed_mps * std::cos(rad), latest_fix_.speed_mps * std::sin(rad));
          main_filter_.update_gnss_velocity(vel_enu, 0.2);
          shadow_filter_.update_gnss_velocity(vel_enu, 0.2);
          vel_res_mps = (vel_enu - main_filter_.state().v.head<2>()).norm();
        }
      }
    }
  }

  // Tangent plane re-anchoring check (§3.4: every 5 km)
  if (tangent_plane_.needs_reanchor(main_filter_.state().p)) {
    double cur_lat, cur_lon, cur_alt;
    tangent_plane_.reverse(main_filter_.state().p, cur_lat, cur_lon, cur_alt);
    tangent_plane_.reset(cur_lat, cur_lon, cur_alt);
    main_filter_.mutable_state().p = Vector3d::Zero();
    shadow_filter_.mutable_state().p = Vector3d::Zero();
  }

  // M3 Map Matching: compute vehicle true heading from orientation R_wv = R * R_bv
  Matrix3d R_wv = main_filter_.state().R * main_filter_.state().R_bv;
  Vector3d fwd_w = R_wv.col(0); // Forward axis in World ENU
  double cur_heading_deg = std::fmod((std::atan2(fwd_w(0), fwd_w(1)) * RAD2DEG + 360.0), 360.0);
  double shadow_sigma = std::sqrt(shadow_filter_.covariance()(6,6));
  auto match_res = map_matcher_.process(shadow_filter_.state().p.head<2>(), cur_heading_deg, shadow_sigma, 0.1);

  if (!is_ins_only && !is_nhc_only && !is_no_map && match_res.is_trusted) {
    main_filter_.update_map_crosstrack(match_res.distance_to_edge_m, match_res.normal_unit, 3.0);
  }

  // Integrity monitor step
  double hpl_m = main_filter_.compute_hpl(3.72);
  auto rep = integrity_monitor_.step(fix_attempted, fix_accepted, pos_nis, vel_res_mps,
                                     median_cn0_dbhz_, agc_change_db_, hpl_m, now_ns, 0.1);

  // Build current state output
  current_state_.t_ns = now_ns;
  if (tangent_plane_.is_initialized()) {
    tangent_plane_.reverse(main_filter_.state().p, current_state_.lat_deg,
                           current_state_.lon_deg, current_state_.alt_m);
  } else if (latest_fix_.valid) {
    current_state_.lat_deg = latest_fix_.lat_deg;
    current_state_.lon_deg = latest_fix_.lon_deg;
    current_state_.alt_m = latest_fix_.alt_m;
  }

  current_state_.speed_mps = main_filter_.state().v.norm();
  current_state_.speed_sigma_mps = std::sqrt(main_filter_.covariance()(3,3));
  current_state_.heading_deg = cur_heading_deg;
  current_state_.mode = static_cast<idr_nav_mode>(rep.mode);
  current_state_.vehicle_mode = (vehicle_classifier_.mode() == vehicle_mode::VehicleMode::CAR)
                                ? IDR_VEHICLE_CAR : IDR_VEHICLE_TWO_WHEELER;
  current_state_.align_state = static_cast<idr_align_state>(alignment_.state());
  current_state_.hpl_m = hpl_m;

  main_filter_.compute_error_ellipse(current_state_.ell_major_m,
                                     current_state_.ell_minor_m,
                                     current_state_.ell_angle_deg);

  current_state_.matched_way_id = match_res.matched_way_id;
  current_state_.matched_edge_id = match_res.matched_edge_id;
  current_state_.match_posterior = match_res.posterior;
  current_state_.floor_level += match_res.floor_change;

  current_state_.flags = 0;
  if (shock) current_state_.flags |= 0x01;
  if (slide) current_state_.flags |= 0x02;
  if (match_res.tunnel_ahead_armed) current_state_.flags |= 0x04;
  if (rep.jamming_detected) current_state_.flags |= 0x08;
}

int NavigationEngine::get_state(idr_state& out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  out = current_state_;
  return 0;
}

int NavigationEngine::poll_event(idr_event& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (event_queue_.empty()) return 0;
  out = event_queue_.front();
  event_queue_.pop_front();
  return 1;
}

void NavigationEngine::emit_event(int type, double lat, double lon, const std::string& payload) {
  idr_event ev;
  ev.t_ns = last_10hz_ns_;
  ev.type = type;
  ev.lat_deg = lat;
  ev.lon_deg = lon;
  std::strncpy(ev.payload_json, payload.c_str(), sizeof(ev.payload_json) - 1);
  ev.payload_json[sizeof(ev.payload_json) - 1] = '\0';
  event_queue_.push_back(ev);
}

int NavigationEngine::save_profile(char* buf, int max_len) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!buf || max_len < 128) return -1;
  Quaterniond q(alignment_.R_bv());
  std::snprintf(buf, max_len,
    "{\"mode\":\"%s\",\"q_bv\":[%.6f,%.6f,%.6f,%.6f],\"s_v\":%.4f}",
    (vehicle_classifier_.mode() == vehicle_mode::VehicleMode::CAR) ? "CAR" : "TWO_WHEELER",
    q.w(), q.x(), q.y(), q.z(), main_filter_.state().speed_scale);
  return 0;
}

int NavigationEngine::load_profile(const char* /*json*/) {
  return 0;
}

} // namespace idr::engine
