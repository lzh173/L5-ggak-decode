#include "ggak_core.h"

#include <fstream>

namespace ggak {
namespace {
constexpr double TICK_SECONDS = 254.84;
constexpr double FINE_STEP = TICK_SECONDS / 256.0;

double frame_time(const std::array<uint8_t, FRAME>& f, const FrameDecodeState& state) {
    return (double(be16(f.data() + 10)) + state.coarse_offset) * TICK_SECONDS + double(f[12]) * FINE_STEP;
}

void decode_spectral(const uint8_t* payload, std::size_t len, uint8_t marker, uint8_t fill,
                     std::vector<std::vector<float>>& output, std::vector<uint8_t>& buffer) {
    std::vector<uint8_t> bytes;
    bytes.reserve(buffer.size() + len);
    bytes.insert(bytes.end(), buffer.begin(), buffer.end());
    bytes.insert(bytes.end(), payload, payload + len);
    buffer.clear();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        while (offset < bytes.size() && bytes[offset] != marker) ++offset;
        if (offset >= bytes.size()) break;
        std::size_t next = offset + 10;
        if (next > bytes.size()) { buffer.insert(buffer.end(), bytes.begin() + offset, bytes.end()); break; }
        while (next < bytes.size() && bytes[next] != marker) ++next;
        std::size_t packet_len = next - offset;
        if (next == bytes.size()) {
            std::size_t trimmed = packet_len;
            while (trimmed > 3 && bytes[offset + trimmed - 1] == fill) --trimmed;
            if (trimmed == packet_len) { buffer.insert(buffer.end(), bytes.begin() + offset, bytes.end()); break; }
            packet_len = trimmed;
        } else {
            while (packet_len > 3 && bytes[offset + packet_len - 1] == fill) --packet_len;
        }
        const std::size_t value_bytes = packet_len - 3;
        if (packet_len >= 10 && value_bytes % 2 == 0) {
            std::vector<float> row;
            row.reserve(value_bytes / 2);
            for (std::size_t i = 0; i < value_bytes; i += 2)
                row.push_back(float(int16_t(be16(bytes.data() + offset + 3 + i))));
            output.push_back(std::move(row));
            offset = next;
        } else {
            ++offset;
        }
    }
}
} // namespace

uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

bool valid(const std::array<uint8_t, FRAME>& frame) {
    uint32_t sum = 0;
    for (std::size_t i = 0; i < FRAME - 2; ++i) sum += frame[i];
    return uint16_t(sum) == be16(frame.data() + FRAME - 2);
}

void decode_frame(const std::array<uint8_t, FRAME>& f, bool strict, Data& d, FrameDecodeState& state) {
    ++d.total;
    const bool ok = valid(f);
    ok ? ++d.pass : ++d.fail;
    if (strict && !ok) return;
    const uint8_t type = f[4];
    ++d.frame_types[type];
    if (type == 0x77 || type == 0x88) { ++d.fill; return; }
    const uint16_t master = be16(f.data() + 5), channel = be16(f.data() + 7);
    if (d.have_master) { const uint16_t delta = uint16_t(master - d.last_master); if (delta > 1 && delta < 60000) { ++d.frame_gaps; d.missing_frames += delta - 1; } }
    d.last_master = master; d.have_master = true;
    if (d.have_channel[type]) { const uint16_t delta = uint16_t(channel - d.last_channel[type]); if (delta > 1 && delta < 60000) { ++d.frame_gaps; d.missing_frames += delta - 1; } }
    d.last_channel[type] = channel; d.have_channel[type] = true;
    if (!d.source) { d.source = f[9]; d.profile = d.source == 0x30 ? "GGAK-E" : (d.source == 0x31 || d.source == 0x33 ? "GGAK-VE" : "GGAK(未知)"); }
    const uint8_t fill = d.source == 0x30 ? 0xaa : 0x33;
    const double time = frame_time(f, state);
    const uint8_t* p = f.data() + 13;
    if (type == 0x70) for (std::size_t i = 0; i < 13; ++i) { auto q = p + i * 15; auto x = be16(q + 2), y = be16(q + 4), z = be16(q + 6); if (x <= 10000 && y <= 10000 && z <= 10000 && q[12] != 0xff) { d.mag[0].push_back(float(int(be16(q)) - 2339) * 2.359f); d.mag[1].push_back(float(int(x) - 2339) * 2.359f); d.mag[2].push_back(float(int(y) - 2339) * 2.359f); d.mag[3].push_back(float(int(z) - 2339) * 2.359f); d.mag_voltage.push_back(float(be16(q + 8)) / 125.0f); d.mag_time.push_back(time); } }
    else if (type == 0x40) for (std::size_t i = 0; i < 11; ++i) { auto q = p + i * 19; if ((q[0] == 0xa0 || q[0] == 0xa1) && q[3] != fill) { for (std::size_t ch = 0; ch < 8; ++ch) d.particle[ch].push_back(float(be16(q + 3 + ch * 2))); d.particle_time.push_back(time); } }
    else if (type == 0x20 || type == 0x30) { auto& out = type == 0x20 ? d.esa_v : d.esa_g; auto& times = type == 0x20 ? d.esa_v_time : d.esa_g_time; const uint8_t marker = type == 0x20 ? 0x90 : 0x98; for (std::size_t o = 0; o <= 138; o += 69) { auto q = p + o; bool all = true; for (std::size_t j = 0; j < 69; ++j) all &= q[j] == fill; if (q[0] == marker && !all) { EsaRow row{}; for (std::size_t b = 0; b < 32; ++b) row[b] = int16_t(be16(q + 3 + b * 2)); out.push_back(row); times.push_back(time); } } }
    else if (type == 0x50 || type == 0x5c) { const std::size_t before = d.skl_spectral.size(); decode_spectral(p, PAYLOAD, type == 0x50 ? 0xa8 : 0xac, fill, d.skl_spectral, state.spectral_buffer); const std::size_t added = d.skl_spectral.size() - before; d.spectral_packets += added; d.spectral_time.insert(d.spectral_time.end(), added, time); }
    else if (type == 0x00 || type == 0x10 || type == 0x60) { const uint8_t marker = type == 0x10 ? 0x88 : 0x80; for (std::size_t i = 0; i + 12 <= PAYLOAD; i += 12) { auto q = p + i; bool all_fill = true; for (std::size_t j = 0; j < 12; ++j) all_fill &= q[j] == fill; if (q[0] != marker || all_fill) continue; auto& channels = type == 0x00 ? d.housekeeping_a : (type == 0x10 ? d.ser_channels : d.housekeeping_b); for (std::size_t ch = 0; ch < 5; ++ch) channels[ch].push_back(float(be16(q + 2 + ch * 2))); const float value = float(be16(q + 6)) * (type == 0x00 ? .0331f : 1.0f); if (type == 0x00) { d.tsi.push_back(value); d.tsi_time.push_back(time); } else if (type == 0x10) { d.ser.push_back(value); d.ser_time.push_back(time); } else ++d.housekeeping_b_count; } }
    state.previous_coarse = be16(f.data() + 10);
}

bool decode_file(const std::string& path, bool strict, Data& data, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) { error = "无法打开文件"; return false; }
    std::array<uint8_t, FRAME> frame{};
    FrameDecodeState state;
    while (input.read(reinterpret_cast<char*>(frame.data()), FRAME)) {
        if (state.previous_coarse >= 0 && state.previous_coarse - int(be16(frame.data() + 10)) > 50000) state.coarse_offset += 65536;
        decode_frame(frame, strict, data, state);
    }
    if (data.total == 0) { error = "文件中没有完整 CADU 帧"; return false; }
    return true;
}

} // namespace ggak
