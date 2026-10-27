// §3.4 Right-Invariant Extended Kalman Filter on SE_2(3) (23 Error States)
#ifndef IDR_FILTER_IEKF_HPP
#define IDR_FILTER_IEKF_HPP

#include "common/types.hpp"
#include "lie/so3.hpp"
#include "lie/se23.hpp"
#include <iostream>
#include <deque>

namespace idr::filter {

// State representation: Nominal state X
struct NominalState {
  Matrix3d R{Matrix3d::Identity()};        // Orientation (World to Body/Vehicle)
  Vector3d v{Vector3d::Zero()};            // Velocity in World ENU (m/s)
  Vector3d p{Vector3d::Zero()};            // Position in World ENU (m)
  Vector3d b_omega{Vector3d::Zero()};      // Gyro bias (rad/s)
  Vector3d b_a{Vector3d::Zero()};          // Accel bias (m/s^2)
  Matrix3d R_bv{Matrix3d::Identity()};     // Mount rotation (Body to Vehicle)
  Vector2d lever_arm{Vector2d::Zero()};    // Rear-contact lever arm (m)
  double speed_scale{1.0};                 // Speed scale factor s_v
  Vector2d map_offset{Vector2d::Zero()};   // Map offset b_m (East, North) in m
};

class RightInvariantIEKF {
public:
  RightInvariantIEKF() {
    reset();
  }

  void reset() {
    state_ = NominalState();
    P_ = MatrixState::Zero();

    // Initialize 23-state covariance diagonal (§3.4)
    P_.block<3,3>(0,0)   = Matrix3d::Identity() * (5.0 * DEG2RAD * 5.0 * DEG2RAD); // Att (5 deg)
    P_.block<3,3>(3,3)   = Matrix3d::Identity() * (0.5 * 0.5);                     // Vel (0.5 m/s)
    P_.block<3,3>(6,6)   = Matrix3d::Identity() * (5.0 * 5.0);                     // Pos (5 m)
    P_.block<3,3>(9,9)   = Matrix3d::Identity() * (0.005 * 0.005);                 // Gyro bias
    P_.block<3,3>(12,12) = Matrix3d::Identity() * (0.05 * 0.05);                   // Accel bias
    P_.block<3,3>(15,15) = Matrix3d::Identity() * (10.0 * DEG2RAD * 10.0 * DEG2RAD);// Mount (10 deg)
    P_.block<2,2>(18,18) = Matrix2d::Identity() * (0.1 * 0.1);                     // Lever arm
    P_(20,20)            = 0.05 * 0.05;                                            // Speed scale (5%)
    P_.block<2,2>(21,21) = Matrix2d::Identity() * (2.0 * 2.0);                     // Map offset (2 m)

    // Process noise Q
    Q_ = MatrixState::Zero();
    Q_.block<3,3>(0,0)   = Matrix3d::Identity() * 1e-4; // Gyro ARW
    Q_.block<3,3>(3,3)   = Matrix3d::Identity() * 1e-3; // Accel VRW
    Q_.block<3,3>(9,9)   = Matrix3d::Identity() * 1e-7; // Gyro bias walk
    Q_.block<3,3>(12,12) = Matrix3d::Identity() * 1e-6; // Accel bias walk
    Q_.block<3,3>(15,15) = Matrix3d::Identity() * 1e-6; // Mount drift
    Q_(20,20)            = 1e-7;                        // Scale factor drift
    Q_.block<2,2>(21,21) = Matrix2d::Identity() * 1e-4; // Map offset random walk
  }

  // High-rate IMU Propagation (10 Hz to 200 Hz) (§3.4)
  void predict(const Vector3d& accel_meas, const Vector3d& gyro_meas, double dt_s) {
    Vector3d omega = gyro_meas - state_.b_omega;
    Vector3d a_body = accel_meas - state_.b_a;

    // Nominal state kinematic propagation
    Matrix3d dR = lie::exp_so3(omega * dt_s);
    Vector3d a_world = state_.R * a_body + Vector3d(0.0, 0.0, -STANDARD_GRAVITY);

    state_.p += state_.v * dt_s + 0.5 * a_world * dt_s * dt_s;
    state_.v += a_world * dt_s;

    // Kinematic speed bounding in prediction (§2.1)
    Matrix3d R_wv_pred = state_.R * state_.R_bv;
    Vector3d v_veh_pred = R_wv_pred.transpose() * state_.v;
    v_veh_pred(0) = std::clamp(v_veh_pred(0), -2.0, 45.0);
    v_veh_pred(1) = std::clamp(v_veh_pred(1), -2.0, 2.0);
    v_veh_pred(2) = std::clamp(v_veh_pred(2), -1.0, 1.0);
    state_.v = R_wv_pred * v_veh_pred;

    state_.R = state_.R * dR;

    // State transition matrix Phi (3rd order expansion)
    MatrixState Phi = MatrixState::Identity();
    // Orientation error: d/dt(xi_R) = - [state.R * omega]_x
    Phi.block<3,3>(0,0) = Matrix3d::Identity() - lie::skew(state_.R * omega) * dt_s;
    Phi.block<3,3>(0,9) = -state_.R * dt_s; // Gyro bias coupling

    // Velocity error: d/dt(xi_v) = - [a_world + g]_x * xi_R + a_world_noise
    Phi.block<3,3>(3,0) = -lie::skew(a_world + Vector3d(0.0, 0.0, STANDARD_GRAVITY)) * dt_s;
    Phi.block<3,3>(3,3) = Matrix3d::Identity();
    Phi.block<3,3>(3,12) = -state_.R * dt_s; // Accel bias coupling

    // Position error: d/dt(xi_p) = xi_v
    Phi.block<3,3>(6,3) = Matrix3d::Identity() * dt_s;
    Phi.block<3,3>(6,6) = Matrix3d::Identity();

    // Map offset correlation decay (L_m correlation length, default ~500m)
    double speed = state_.v.norm();
    double decay = std::exp(-speed * dt_s / 500.0);
    Phi.block<2,2>(21,21) = Matrix2d::Identity() * decay;

    // Covariance propagation: P = Phi * P * Phi^T + Q * dt
    P_ = Phi * P_ * Phi.transpose() + Q_ * dt_s;
    // Enforce symmetry
    P_ = 0.5 * (P_ + P_.transpose()).eval();
  }

  // Non-Holonomic Constraints (NHC) update: v_y^v = 0, v_z^v = 0 (§3.4)
  // R_NHC = diag( alpha_1 * (sigma_0 + c*|a_y|)^2, alpha_2 * sigma_up^2 )
  double update_nhc(double alpha1, double alpha2, double a_lat, bool slide_or_shock) {
    if (slide_or_shock) return 0.0; // Suspended during slides or road shocks

    // Vehicle frame velocity: v_veh = R_bv^T * R^T * v_world
    Matrix3d R_wv = state_.R * state_.R_bv;
    Vector3d v_veh = R_wv.transpose() * state_.v;

    // Residual: z = [v_y^v, v_z^v]
    Vector2d z(-v_veh(1), -v_veh(2));

    // Measurement Jacobian H (2 x 23)
    Eigen::Matrix<double, 2, STATE_DIM> H = Eigen::Matrix<double, 2, STATE_DIM>::Zero();
    // With respect to world velocity
    H.block<2,3>(0,3) = R_wv.transpose().bottomRows<2>();

    // Measurement noise R_NHC
    double sigma_0 = 0.05;
    double c = 0.02;
    double sigma_up = 0.05;
    double r_lat = alpha1 * std::pow(sigma_0 + c * std::abs(a_lat), 2);
    double r_up  = alpha2 * std::pow(sigma_up, 2);
    Matrix2d R = Matrix2d::Zero();
    R(0,0) = std::max(1e-4, r_lat);
    R(1,1) = std::max(1e-4, r_up);

    double nis = apply_update<2>(z, H, R, 13.8); // Chi-squared 99.9% gate for 2 DOF = 13.8

    // Post-update lateral/vertical constraint
    Matrix3d R_wv_post = state_.R * state_.R_bv;
    Vector3d v_post = R_wv_post.transpose() * state_.v;
    v_post(1) = std::clamp(v_post(1), -1.5, 1.5);
    v_post(2) = std::clamp(v_post(2), -0.5, 0.5);
    state_.v = R_wv_post * v_post;

    return nis;
  }

  // Speed TCN update: v_x^v = s_v * v_tcn (§3.4)
  double update_speed(double v_tcn, double var_tcn, double alpha3, bool shock_flag) {
    if (shock_flag) return 0.0;

    Matrix3d R_wv = state_.R * state_.R_bv;
    Vector3d v_veh = R_wv.transpose() * state_.v;

    double pred_speed = v_veh(0);
    double target_speed = state_.speed_scale * v_tcn;
    double z = target_speed - pred_speed;

    Eigen::Matrix<double, 1, STATE_DIM> H = Eigen::Matrix<double, 1, STATE_DIM>::Zero();
    H.block<1,3>(0,3) = R_wv.transpose().row(0); // d(v_x^v)/dv
    H(0,20) = -v_tcn;                            // d(z)/d(s_v) with negative feedback to converge s_v

    Eigen::Matrix<double, 1, 1> R;
    R(0,0) = std::max(1e-4, alpha3 * var_tcn);

    Eigen::Matrix<double, 1, 1> z_vec;
    z_vec(0,0) = z;

    double res_nis = apply_update<1>(z_vec, H, R, 25.0);

    // Kinematic bound: clamp forward speed within 3-sigma envelope of Speed TCN (§2.1)
    Matrix3d R_wv_post = state_.R * state_.R_bv;
    Vector3d v_post = R_wv_post.transpose() * state_.v;
    double max_dev = 3.0 * std::max(0.3, std::sqrt(var_tcn));
    v_post(0) = std::clamp(v_post(0), std::max(0.0, target_speed - max_dev), target_speed + max_dev);
    v_post(1) = std::clamp(v_post(1), -1.5, 1.5);
    v_post(2) = std::clamp(v_post(2), -0.5, 0.5);
    state_.v = R_wv_post * v_post;

    return res_nis;
  }

  // Zero Velocity Update (ZUPT): v_world = 0 (§3.2, §3.4)
  void update_zupt(double sigma_zupt = 0.01) {
    Vector3d z = -state_.v;
    Eigen::Matrix<double, 3, STATE_DIM> H = Eigen::Matrix<double, 3, STATE_DIM>::Zero();
    H.block<3,3>(0,3) = Matrix3d::Identity();

    Matrix3d R = Matrix3d::Identity() * (sigma_zupt * sigma_zupt);
    apply_update<3>(z, H, R, 100.0);
    state_.v = Vector3d::Zero();
  }

  // Zero Angular Rate Update (ZARU): omega_body = 0 (§3.2, §3.4)
  void update_zaru(const Vector3d& omega_body, double sigma_zaru = 0.005) {
    Vector3d z = omega_body - state_.b_omega;
    Eigen::Matrix<double, 3, STATE_DIM> H = Eigen::Matrix<double, 3, STATE_DIM>::Zero();
    H.block<3,3>(0,9) = Matrix3d::Identity(); // Gyro bias

    Matrix3d R = Matrix3d::Identity() * (sigma_zaru * sigma_zaru);
    apply_update<3>(z, H, R, 100.0);
  }

  // Leveling update from accelerometer gravity vector (§3.1)
  // Aligns estimated attitude with the true gravity vertical
  void update_gravity_level(const Vector3d& accel_body, double sigma_level = 0.02) {
    Vector3d f_unit = accel_body.normalized();
    Vector3d g_world_pred = state_.R * f_unit;
    Vector3d g_world_true(0.0, 0.0, 1.0);
    Vector2d z = g_world_true.head<2>() - g_world_pred.head<2>();

    Eigen::Matrix<double, 2, STATE_DIM> H = Eigen::Matrix<double, 2, STATE_DIM>::Zero();
    Matrix3d skew_g = -lie::skew(g_world_pred);
    H.block<2,3>(0,0) = skew_g.topRows<2>();

    Matrix2d R = Matrix2d::Identity() * (sigma_level * sigma_level);
    apply_update<2>(z, H, R, 50.0);
  }

  // GNSS Position update in local ENU (m) (§3.4)
  // R = (r68 / 1.51)^2 * I_2
  double update_gnss_position(const Vector2d& pos_enu, double r68_m, double gate_threshold = 13.8) {
    Vector2d z = pos_enu - state_.p.head<2>();
    Eigen::Matrix<double, 2, STATE_DIM> H = Eigen::Matrix<double, 2, STATE_DIM>::Zero();
    H.block<2,2>(0,6) = Matrix2d::Identity();

    double sigma_pos = r68_m / 1.51;
    Matrix2d R = Matrix2d::Identity() * std::max(0.25, sigma_pos * sigma_pos);

    return apply_update<2>(z, H, R, gate_threshold);
  }

  // GNSS Velocity update in local ENU (m) (§3.4)
  double update_gnss_velocity(const Vector2d& vel_enu, double sigma_vel = 0.2) {
    Vector2d z = vel_enu - state_.v.head<2>();
    Eigen::Matrix<double, 2, STATE_DIM> H = Eigen::Matrix<double, 2, STATE_DIM>::Zero();
    H.block<2,2>(0,3) = Matrix2d::Identity();

    Matrix2d R = Matrix2d::Identity() * (sigma_vel * sigma_vel);
    return apply_update<2>(z, H, R, 13.8);
  }

  // Map Cross-Track soft update (§3.3)
  // z: distance from road centreline perpendicular to edge heading
  double update_map_crosstrack(double distance_m, const Vector2d& normal_unit, double sigma_map_m) {
    Eigen::Matrix<double, 1, 1> z_vec;
    z_vec(0, 0) = distance_m;

    Eigen::Matrix<double, 1, STATE_DIM> H = Eigen::Matrix<double, 1, STATE_DIM>::Zero();
    H(0,6) = normal_unit(0);
    H(0,7) = normal_unit(1);
    // Map offset state coupling b_m
    H(0,21) = -normal_unit(0);
    H(0,22) = -normal_unit(1);

    Eigen::Matrix<double, 1, 1> R;
    R(0,0) = std::max(0.5, sigma_map_m * sigma_map_m);

    return apply_update<1>(z_vec, H, R, 10.8);
  }

  const NominalState& state() const { return state_; }
  NominalState& mutable_state() { return state_; }
  const MatrixState& covariance() const { return P_; }

  // Horizontal Protection Level: HPL = k_H * sqrt(lambda_max(P_hh)) with k_H = 3.72 (§3.4)
  double compute_hpl(double k_H = 3.72) const {
    Matrix2d P_pos = P_.block<2,2>(6,6);
    Eigen::SelfAdjointEigenSolver<Matrix2d> es(P_pos);
    double lambda_max = es.eigenvalues().maxCoeff();
    return k_H * std::sqrt(std::max(0.0, lambda_max));
  }

  // Error ellipse parameters: semi-major, semi-minor, orientation angle
  void compute_error_ellipse(double& semi_major_m, double& semi_minor_m, double& angle_deg) const {
    Matrix2d P_pos = P_.block<2,2>(6,6);
    Eigen::SelfAdjointEigenSolver<Matrix2d> es(P_pos);
    Vector2d ev = es.eigenvalues();
    semi_major_m = std::sqrt(std::max(0.0, ev(1))) * 2.45; // 95% ellipse
    semi_minor_m = std::sqrt(std::max(0.0, ev(0))) * 2.45;
    Vector2d max_vec = es.eigenvectors().col(1);
    angle_deg = std::atan2(max_vec(1), max_vec(0)) * RAD2DEG;
  }

private:
  // Joseph-form stabilized Kalman update for arbitrary dimension M
  template <int M>
  double apply_update(const Eigen::Matrix<double, M, 1>& z,
                      const Eigen::Matrix<double, M, STATE_DIM>& H,
                      const Eigen::Matrix<double, M, M>& R,
                      double gate_threshold) {
    // Innovation gating with robust Huber weighting
    Eigen::Matrix<double, M, M> S = H * P_ * H.transpose() + R;
    double nis = (z.transpose() * S.inverse() * z)(0,0);

    Eigen::Matrix<double, M, M> R_eff = R;
    if (nis > gate_threshold) {
      // Huber pseudo-weighting: scale R by nis / gate
      double scale = std::min(100.0, nis / gate_threshold);
      R_eff = R * scale;
      S = H * P_ * H.transpose() + R_eff;
    }

    // Kalman gain K = P * H^T * S^-1
    Eigen::Matrix<double, STATE_DIM, M> K = P_ * H.transpose() * S.inverse();
    VectorState dx = K * z;

    // Apply error state correction
    // Orientation: R = R * exp(dx_R)
    state_.R = state_.R * lie::exp_so3(dx.segment<3>(0));
    state_.v += dx.segment<3>(3);
    state_.p += dx.segment<3>(6);
    state_.b_omega += dx.segment<3>(9);
    state_.b_a += dx.segment<3>(12);
    state_.R_bv = state_.R_bv * lie::exp_so3(dx.segment<3>(15));
    state_.lever_arm += dx.segment<2>(18);
    state_.speed_scale += dx(20);
    state_.speed_scale = std::clamp(state_.speed_scale, 0.85, 1.15);
    state_.map_offset += dx.segment<2>(21);

    // Kinematic speed bounding (§2.1)
    Matrix3d R_wv = state_.R * state_.R_bv;
    Vector3d v_veh = R_wv.transpose() * state_.v;
    v_veh(0) = std::clamp(v_veh(0), -2.0, 45.0);
    v_veh(1) = std::clamp(v_veh(1), -5.0, 5.0);
    v_veh(2) = std::clamp(v_veh(2), -2.0, 2.0);
    state_.v = R_wv * v_veh;

    // Joseph form covariance update: P = (I - K*H)*P*(I - K*H)^T + K*R_eff*K^T
    MatrixState I_KH = MatrixState::Identity() - K * H;
    P_ = I_KH * P_ * I_KH.transpose() + K * R_eff * K.transpose();
    P_ = 0.5 * (P_ + P_.transpose()).eval();

    return nis;
  }

  NominalState state_;
  MatrixState P_;
  MatrixState Q_;
};

} // namespace idr::filter

#endif // IDR_FILTER_IEKF_HPP
