// §2.1 & §3.4 M4 Noise Adapter Inference Runner (6,949 parameters)
#ifndef IDR_FILTER_NOISE_ADAPTER_HPP
#define IDR_FILTER_NOISE_ADAPTER_HPP

#include "common/types.hpp"
#include <vector>
#include <string>
#include <cmath>
#include <deque>

#if __has_include(<onnxruntime_cxx_api.h>)
#include <onnxruntime_cxx_api.h>
#endif

namespace idr::filter {

struct NoiseScales {
  double alpha_nhc_lat{1.0};  // alpha_1: scales R_NHC(lat)
  double alpha_nhc_up{1.0};   // alpha_2: scales R_NHC(up)
  double alpha_speed{1.0};    // alpha_3: scales R_speed
  double alpha_process{1.0};  // alpha_4: scales Q_a (accel process noise)
  double alpha_gyro{1.0};     // alpha_5: scales Q_omega (gyro process noise)
};

class NoiseAdapterRunner {
public:
  NoiseAdapterRunner() = default;

  bool load_model(const std::string& onnx_path) {
#if IDR_HAS_ORT
    try {
      env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "NoiseAdapter");
      Ort::SessionOptions options;
      options.SetIntraOpNumThreads(1);
      options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

#if defined(_WIN32)
      std::wstring wpath(onnx_path.begin(), onnx_path.end());
      session_ = std::make_unique<Ort::Session>(*env_, wpath.c_str(), options);
#else
      session_ = std::make_unique<Ort::Session>(*env_, onnx_path.c_str(), options);
#endif
      is_loaded_ = true;
      return true;
    } catch (...) {
      is_loaded_ = false;
      return false;
    }
#else
    (void)onnx_path;
    return false;
#endif
  }

  // Push 10 channels: f^v(3), omega^v(3), ell_fz(1), ell_fy(1), ell_wx(1), mu_speed(1)
  void push_features(const Vector3d& f_v, const Vector3d& omega_v,
                     double ell_fz, double ell_fy, double ell_wx, double mu_speed) {
    std::array<float, 10> sample = {
      static_cast<float>(f_v(0)), static_cast<float>(f_v(1)), static_cast<float>(f_v(2)),
      static_cast<float>(omega_v(0)), static_cast<float>(omega_v(1)), static_cast<float>(omega_v(2)),
      static_cast<float>(ell_fz), static_cast<float>(ell_fy), static_cast<float>(ell_wx),
      static_cast<float>(mu_speed)
    };
    window_.push_back(sample);
    if (window_.size() > 30) {
      window_.pop_front();
    }
  }

  // Run inference or analytical vibration adaptation
  // alpha_i = 10^(3 * tanh(z_i)) (§2.1)
  NoiseScales predict() {
    NoiseScales scales;
    if (window_.size() < 5) return scales;

#if IDR_HAS_ORT
    if (is_loaded_ && session_) {
      try {
        std::vector<float> input_tensor(1 * 10 * 30, 0.0f);
        size_t n = window_.size();
        size_t offset = 30 - n;
        for (size_t t = 0; t < n; ++t) {
          for (size_t c = 0; c < 10; ++c) {
            input_tensor[c * 30 + (offset + t)] = window_[t][c];
          }
        }

        std::array<int64_t, 3> input_shape = {1, 10, 30};
        Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value input_val = Ort::Value::CreateTensor<float>(
            mem_info, input_tensor.data(), input_tensor.size(), input_shape.data(), input_shape.size());

        const char* input_names[] = {"input"};
        const char* output_names[] = {"output"};

        auto output_tensors = session_->Run(
            Ort::RunOptions{nullptr}, input_names, &input_val, 1, output_names, 1);

        float* out_data = output_tensors.front().GetTensorMutableData<float>();

        // alpha = 10^(3 * tanh(z))
        scales.alpha_nhc_lat = std::pow(10.0, 3.0 * std::tanh(out_data[0]));
        scales.alpha_nhc_up  = std::pow(10.0, 3.0 * std::tanh(out_data[1]));
        scales.alpha_speed   = std::pow(10.0, 3.0 * std::tanh(out_data[2]));
        scales.alpha_process = std::pow(10.0, 3.0 * std::tanh(out_data[3]));
        scales.alpha_gyro    = std::pow(10.0, 3.0 * std::tanh(out_data[4]));
        return scales;
      } catch (...) {
        // Fallback
      }
    }
#endif

    // Analytical adaptation based on road vibration envelope (§3.2)
    double ell_fz = window_.back()[6];
    double roughness = std::clamp(std::pow(10.0, ell_fz), 0.1, 10.0);

    scales.alpha_nhc_lat = roughness;
    scales.alpha_nhc_up  = roughness * 1.5;
    scales.alpha_speed   = std::clamp(roughness * 0.8, 0.5, 5.0);
    scales.alpha_process = std::sqrt(roughness);

    return scales;
  }

  bool is_loaded() const { return is_loaded_; }

private:
  bool is_loaded_{false};
  std::deque<std::array<float, 10>> window_;

#if IDR_HAS_ORT
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
#endif
};

} // namespace idr::filter

#endif // IDR_FILTER_NOISE_ADAPTER_HPP
