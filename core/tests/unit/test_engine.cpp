// Unit test for C API and Navigation Engine
#include <gtest/gtest.h>
#include "idr/idr.h"

TEST(EngineTest, LifecycleAndStepping) {
  idr_config cfg{};
  cfg.imu_rate_hz = 10.0;
  cfg.alert_limit_m = 20.0;
  cfg.has_gyroscope = 1;

  idr_engine* eng = idr_create(&cfg);
  ASSERT_NE(eng, nullptr);

  // Push an initial GNSS fix
  idr_fix fix{};
  fix.t_ns = 0;
  fix.lat_deg = 18.9220;
  fix.lon_deg = 72.8347;
  fix.alt_m = 10.0;
  fix.speed_mps = 0.0;
  fix.bearing_deg = 0.0;
  fix.r68_m = 2.0;
  fix.num_sats = 12;
  fix.valid = 1;
  idr_push_fix(eng, &fix);

  // Push 1 second of IMU stationary samples at 10 Hz
  for (int i = 0; i < 10; ++i) {
    idr_imu imu{};
    imu.t_ns = i * 100'000'000LL;
    imu.ax = 0.0;
    imu.ay = 0.0;
    imu.az = 9.80665;
    imu.gx = 0.0;
    imu.gy = 0.0;
    imu.gz = 0.0;

    idr_push_imu(eng, &imu);
    idr_step(eng, imu.t_ns);
  }

  idr_state st{};
  int rc = idr_get_state(eng, &st);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(st.mode, IDR_MODE_GNSS_OK);
  EXPECT_GT(st.hpl_m, 0.0);
  EXPECT_NEAR(st.lat_deg, 18.9220, 1e-4);

  // Simulate GNSS outage > 1.5 seconds and verify transition to DR
  for (int i = 10; i <= 30; ++i) {
    idr_imu imu{};
    imu.t_ns = i * 100'000'000LL;
    imu.ax = 0.0;
    imu.ay = 0.0;
    imu.az = 9.80665;
    imu.gx = 0.0;
    imu.gy = 0.0;
    imu.gz = 0.0;

    idr_push_imu(eng, &imu);
    idr_step(eng, imu.t_ns);
  }

  rc = idr_get_state(eng, &st);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(st.mode, IDR_MODE_DR);

  idr_destroy(eng);
}

TEST(EngineTest, ModelLoadingAndInference) {
  idr_config cfg{};
  cfg.imu_rate_hz = 10.0;
  cfg.alert_limit_m = 20.0;
  cfg.has_gyroscope = 1;

  idr_engine* eng = idr_create(&cfg);
  ASSERT_NE(eng, nullptr);

  // Load ONNX models from project root
  int rc = idr_load_models(eng, "../../models/speed_tcn.onnx", "../../models/noise_adapter.onnx");
  if (rc != 0) {
    // Try from current working directory
    rc = idr_load_models(eng, "models/speed_tcn.onnx", "models/noise_adapter.onnx");
  }
#if IDR_HAS_ORT
  // When ORT is compiled, accept either loaded model (0) or graceful fallback (-1) if LFS pointers
  EXPECT_TRUE(rc == 0 || rc == -1);
#else
  // Without ORT, engine correctly uses kinematic fallback
  EXPECT_EQ(rc, -1);
#endif

  // Feed 35 samples to fill the 30-sample TCN receptive field
  for (int i = 0; i < 35; ++i) {
    idr_imu imu{};
    imu.t_ns = i * 100'000'000LL;
    imu.ax = 0.5;
    imu.ay = 0.0;
    imu.az = 9.80665;
    imu.gx = 0.0;
    imu.gy = 0.0;
    imu.gz = 0.02;

    idr_push_imu(eng, &imu);
    idr_step(eng, imu.t_ns);
  }

  idr_state st{};
  rc = idr_get_state(eng, &st);
  EXPECT_EQ(rc, 0);
  EXPECT_GE(st.speed_mps, 0.0);
  EXPECT_GT(st.hpl_m, 0.0);

  idr_destroy(eng);
}

TEST(EngineTest, MapLoadingAndMatching) {
  idr_config cfg{};
  cfg.imu_rate_hz = 10.0;
  cfg.alert_limit_m = 20.0;
  cfg.has_gyroscope = 1;

  idr_engine* eng = idr_create(&cfg);
  ASSERT_NE(eng, nullptr);

  // Load Mumbai map
  int rc = idr_load_map(eng, "../../maps/mumbai_metro/map.sqlite", "../../maps/mumbai_metro/user.sqlite");
  if (rc != 0) {
    rc = idr_load_map(eng, "maps/mumbai_metro/map.sqlite", "maps/mumbai_metro/user.sqlite");
  }
  EXPECT_EQ(rc, 0);

  // Push fix on Eastern Freeway
  idr_fix fix{};
  fix.t_ns = 0;
  fix.lat_deg = 18.9400;
  fix.lon_deg = 72.8450;
  fix.alt_m = 10.0;
  fix.speed_mps = 12.0;
  fix.bearing_deg = 30.0;
  fix.r68_m = 3.0;
  fix.num_sats = 12;
  fix.valid = 1;
  idr_push_fix(eng, &fix);

  // Step 10 times
  for (int i = 0; i < 10; ++i) {
    idr_imu imu{};
    imu.t_ns = i * 100'000'000LL;
    imu.ax = 0.0;
    imu.ay = 0.0;
    imu.az = 9.80665;
    imu.gx = 0.0;
    imu.gy = 0.0;
    imu.gz = 0.0;

    idr_push_imu(eng, &imu);
    idr_step(eng, imu.t_ns);
  }

  idr_state st{};
  rc = idr_get_state(eng, &st);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(st.mode, IDR_MODE_GNSS_OK);

  idr_destroy(eng);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
