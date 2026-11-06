#include "dva/output.hpp"

#include <vector>

#include "dva/ma.hpp"

namespace dva {

struct AudioOutput::Impl {
  ma_context context{};
  ma_device device{};
  bool context_open = false, device_open = false;
  Player* player = nullptr;
  std::string name;
};

namespace {
void callback(ma_device* dev, void* output, const void*, ma_uint32 frames) {
  const auto* impl = static_cast<AudioOutput::Impl*>(dev->pUserData);
  impl->player->render(static_cast<float*>(output), static_cast<int>(frames));
}
}  // namespace

AudioOutput::AudioOutput() : impl_(std::make_unique<Impl>()) {}
AudioOutput::~AudioOutput() { stop(); }

std::string AudioOutput::device_name() const { return impl_->name; }

void AudioOutput::stop() {
  if (impl_->device_open) {
    ma_device_uninit(&impl_->device);
    impl_->device_open = false;
  }
  if (impl_->context_open) {
    ma_context_uninit(&impl_->context);
    impl_->context_open = false;
  }
}

bool AudioOutput::start(Player& player, const std::string& hint, std::string& error) {
  stop();
  if (ma_context_init(nullptr, 0, nullptr, &impl_->context) != MA_SUCCESS) {
    error = "The computer's sound system could not be reached.";
    return false;
  }
  impl_->context_open = true;
  ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
  cfg.playback.format = ma_format_f32;
  cfg.playback.channels = 2;
  cfg.sampleRate = static_cast<ma_uint32>(player.sample_rate());
  cfg.dataCallback = callback;
  cfg.pUserData = impl_.get();
  cfg.periodSizeInMilliseconds = 20;
  cfg.performanceProfile = ma_performance_profile_conservative;
  ma_device_id chosen{};
  if (!hint.empty()) {
    ma_device_info* infos = nullptr;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&impl_->context, &infos, &count, nullptr, nullptr) == MA_SUCCESS) {
      for (ma_uint32 i = 0; i < count; i++) {
        if (std::string(infos[i].name).find(hint) != std::string::npos) {
          chosen = infos[i].id;
          cfg.playback.pDeviceID = &chosen;
          break;
        }
      }
    }
  }
  impl_->player = &player;
  if (ma_device_init(&impl_->context, &cfg, &impl_->device) != MA_SUCCESS) {
    error = "No sound output could be opened.";
    stop();
    return false;
  }
  impl_->device_open = true;
  impl_->name = impl_->device.playback.name;
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    error = "The sound output could not be started.";
    stop();
    return false;
  }
  return true;
}

}  // namespace dva
