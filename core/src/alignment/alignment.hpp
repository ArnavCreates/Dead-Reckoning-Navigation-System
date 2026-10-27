// §2.2 & §3.1 M1 Alignment & Slip Gating Engine
#ifndef IDR_ALIGNMENT_ALIGNMENT_HPP
#define IDR_ALIGNMENT_ALIGNMENT_HPP

#include "common/types.hpp"
#include "lie/so3.hpp"
#include "geo/igrf.hpp"
#include <deque>

#ifdef UNALIGNED
#undef UNALIGNED
#endif

namespace idr::alignment {

enum class AlignState {
  STATE_UNALIGNED = 0,
  STATE_COARSE    = 1,
  STATE_ALIGNED   = 2,
  STATE_SUSPECT   = 3,
  STATE_MOVED     = 4
};

class AlignmentEngine {
public:
  AlignmentEngine() {
    reset();
  }

  void reset() {
    state_ = AlignState::STATE_UNALIGNED;
    R_bv_ = Matrix3d::Identity();
    up_vector_ = Vector3d(0.0, 0.0, 1.0);
    z_mean_ = Vector3d(0.0, 0.0, 1.0);
    sigma_theta_deg_ = 30.0; // Initial uncertainty
    cusum_c_ = 0.0;
    suspect_timer_s_ = 0.0;
    tilt_buffer_.clear();
  }

  // Update up-tracker on sphere (§3.1)
  // Gain kappa_i = (dt/tau) * exp(-(||f^c_i|| - g)^2 / (2 * sigma_g^2))
  // tau = 3 s, sigma_g = 0.05 g
  void update_up_tracker(const Vector3d& f_body, double dt_s, bool is_stationary) {
    if (is_stationary) {
      // Static start / stationary update: direct acceleration mean
      Vector3d g_meas = f_body.normalized();
      up_vector_ = (0.95 * up_vector_ + 0.05 * g_meas).normalized();
    } else {
      // Dynamic moving start complementary tracker
      double f_norm = f_body.norm();
      double diff_g = f_norm - STANDARD_GRAVITY;
      double sigma_g = 0.05 * STANDARD_GRAVITY;
      double tau = 3.0; // 3 seconds
      double gain = (dt_s / tau) * std::exp(-(diff_g * diff_g) / (2.0 * sigma_g * sigma_g));

      Vector3d g_meas = f_body.normalized();
      up_vector_ = (up_vector_ + gain * (g_meas - up_vector_)).normalized();
    }

    // Update 60s moving average z_mean
    z_mean_ = (0.998 * z_mean_ + 0.002 * up_vector_).normalized();
  }

  // Refine forward and lateral axes from turns and horizontal acceleration
  void update_axes_from_motion(const Vector3d& f_body, const Vector3d& omega_body, double speed_mps) {
    if (state_ == AlignState::STATE_SUSPECT || state_ == AlignState::STATE_MOVED) return;

    double yaw_rate = omega_body.dot(up_vector_);
    if (std::abs(yaw_rate) > 0.02 && speed_mps > 2.0) {
      // Centripetal acceleration: a_lat = v * r along vehicle Y axis
      Vector3d a_centripetal = f_body - f_body.dot(up_vector_) * up_vector_;
      if (a_centripetal.norm() > 0.1) {
        Vector3d lat_axis = (yaw_rate > 0) ? a_centripetal.normalized() : (-a_centripetal).normalized();
        Vector3d fwd_axis = lat_axis.cross(up_vector_).normalized();

        Matrix3d R_candidate;
        R_candidate.col(0) = fwd_axis;
        R_candidate.col(1) = lat_axis;
        R_candidate.col(2) = up_vector_;

        // Smoothly blend mount rotation
        double alpha = (state_ == AlignState::STATE_UNALIGNED) ? 0.05 : 0.01;
        R_bv_ = R_bv_ * lie::exp_so3(alpha * lie::log_so3(R_bv_.transpose() * R_candidate));

        if (sigma_theta_deg_ > 1.5) {
          sigma_theta_deg_ *= 0.98;
        }
      }
    }

    // State transitions based on mount uncertainty sigma_theta
    if (state_ == AlignState::STATE_UNALIGNED && sigma_theta_deg_ < 10.0) {
      state_ = AlignState::STATE_COARSE;
    } else if (state_ == AlignState::STATE_COARSE && sigma_theta_deg_ < 2.0) {
      state_ = AlignState::STATE_ALIGNED;
    }
  }

  // Slip gating checks at 10 Hz (§2.2, §3.1)
  // Evaluates tilt change, NHC CUSUM, and stationary rotations
  void check_slip(double nhc_nis, bool is_stationary, double dt_s) {
    tilt_buffer_.push_back(up_vector_);
    if (tilt_buffer_.size() > 20) { // 2 seconds @ 10 Hz
      tilt_buffer_.pop_front();
    }

    // Statistic 1: Tilt change over 2s window > 5 deg
    double tilt_change_deg = 0.0;
    if (tilt_buffer_.size() >= 20) {
      double cos_tilt = tilt_buffer_.front().dot(tilt_buffer_.back());
      cos_tilt = std::clamp(cos_tilt, -1.0, 1.0);
      tilt_change_deg = std::acos(cos_tilt) * RAD2DEG;
    }

    // Statistic 2: CUSUM of NHC NIS (nu0 = 3, threshold h = 20)
    cusum_c_ = std::max(0.0, cusum_c_ + (nhc_nis - 3.0));

    bool trigger_suspect = (tilt_change_deg > 5.0) || (cusum_c_ > 20.0);

    if (state_ == AlignState::STATE_ALIGNED && trigger_suspect) {
      state_ = AlignState::STATE_SUSPECT;
      suspect_timer_s_ = 3.0; // 3 second confirmation window
    } else if (state_ == AlignState::STATE_SUSPECT) {
      suspect_timer_s_ -= dt_s;
      if (trigger_suspect) {
        // Confirmed slip -> MOVED: alert listeners, reset covariance and restart alignment
        state_ = AlignState::STATE_MOVED;
        moved_timer_s_ = 1.0; // Hold STATE_MOVED so callers / engine output observe the slip event
        sigma_theta_deg_ = 30.0;
        cusum_c_ = 0.0;
      } else if (suspect_timer_s_ <= 0.0) {
        // Not confirmed -> return to ALIGNED
        state_ = AlignState::STATE_ALIGNED;
        cusum_c_ = 0.0;
      }
    } else if (state_ == AlignState::STATE_MOVED) {
      moved_timer_s_ -= dt_s;
      if (moved_timer_s_ <= 0.0) {
        state_ = AlignState::STATE_UNALIGNED;
      }
    }
  }

  // Tilt-compensated Magnetometer Start-up Prior with IGRF gates (§3.1)
  bool evaluate_magnetometer_prior(const Vector3d& m_body, const geo::MagneticFieldEstimate& igrf, double& heading_prior_deg) {
    if (!igrf.valid) return false;

    // Field intensity gate: | ||m|| - B_IGRF | < 10 uT
    double m_norm = m_body.norm();
    if (std::abs(m_norm - igrf.total_intensity_ut) >= 10.0) {
      return false;
    }

    // Tilt compensation using established up vector
    Vector3d m_horiz = m_body - m_body.dot(up_vector_) * up_vector_;
    if (m_horiz.norm() < 1e-4) return false;

    // Dip angle gate: |I_m - I_IGRF| < 5 deg
    double dip_angle_deg = std::atan2(-m_body.dot(up_vector_), m_horiz.norm()) * RAD2DEG;
    if (std::abs(dip_angle_deg - igrf.inclination_deg) >= 5.0) {
      return false;
    }

    // Heading calculation relative to vehicle frame forward axis
    Vector3d fwd_body = R_bv_.col(0);
    Vector3d lat_body = R_bv_.col(1);
    double m_fwd = m_horiz.dot(fwd_body);
    double m_lat = m_horiz.dot(lat_body);

    double mag_heading_deg = std::atan2(-m_lat, m_fwd) * RAD2DEG;
    // Add magnetic declination D from IGRF
    heading_prior_deg = std::fmod(mag_heading_deg + igrf.declination_deg + 360.0, 360.0);
    return true;
  }

  AlignState state() const { return state_; }
  Matrix3d R_bv() const { return R_bv_; }
  Vector3d up_vector() const { return up_vector_; }
  double sigma_theta_deg() const { return sigma_theta_deg_; }

private:
  AlignState state_{AlignState::STATE_UNALIGNED};
  Matrix3d R_bv_{Matrix3d::Identity()};
  Vector3d up_vector_{0.0, 0.0, 1.0};
  Vector3d z_mean_{0.0, 0.0, 1.0};
  double sigma_theta_deg_{30.0};
  double cusum_c_{0.0};
  double suspect_timer_s_{0.0};
  double moved_timer_s_{0.0};
  std::deque<Vector3d> tilt_buffer_;
};

} // namespace idr::alignment

#endif // IDR_ALIGNMENT_ALIGNMENT_HPP
