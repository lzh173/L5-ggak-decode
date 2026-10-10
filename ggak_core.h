#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ggak {

constexpr std::size_t FRAME = 224;
constexpr std::size_t PAYLOAD = 209;
constexpr std::size_t ESA_BINS = 32;
using EsaRow = std::array<int16_t, ESA_BINS>;

struct Data {
    uint64_t total = 0, pass = 0, fail = 0, fill = 0;
    int source = 0;
    std::string profile = "未知";
    std::array<std::vector<float>, 4> mag;
    std::vector<float> mag_voltage;
    std::array<std::vector<float>, 8> particle;
    std::vector<float> tsi, ser;
    std::vector<EsaRow> esa_v, esa_g;
    std::array<std::vector<float>, 5> housekeeping_a, housekeeping_b, ser_channels;
    std::vector<std::vector<float>> skl_spectral;
    std::vector<double> mag_time, particle_time, tsi_time, ser_time, esa_v_time, esa_g_time, spectral_time;
    uint64_t spectral_packets = 0, housekeeping_b_count = 0, frame_gaps = 0, missing_frames = 0;
    std::array<uint64_t, 256> frame_types{};
    std::array<uint16_t, 256> last_channel{};
    std::array<bool, 256> have_channel{};
    uint16_t last_master = 0;
    bool have_master = false;
};

struct FrameDecodeState {
    int previous_coarse = -1;
    uint32_t coarse_offset = 0;
    std::vector<uint8_t> spectral_buffer;
};

uint16_t be16(const uint8_t* p);
bool valid(const std::array<uint8_t, FRAME>& frame);
void decode_frame(const std::array<uint8_t, FRAME>& frame, bool strict, Data& data, FrameDecodeState& state);
bool decode_file(const std::string& path, bool strict, Data& data, std::string& error);

} // namespace ggak
