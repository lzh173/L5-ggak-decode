#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr std::size_t CADU_SIZE = 224;
constexpr std::size_t PAYLOAD_OFFSET = 13;
constexpr std::size_t PAYLOAD_SIZE = 209;
constexpr double TIME_TICK_SECONDS = 254.84;
constexpr double FINE_TIME_STEP = TIME_TICK_SECONDS / 256.0;

constexpr std::size_t MAG_PER_FRAME = 13;
constexpr std::size_t MAG_LEN = 15;
constexpr std::size_t PARTICLE_PER_FRAME = 11;
constexpr std::size_t PARTICLE_LEN = 19;
constexpr std::size_t ESA_SIZE = 69;
constexpr std::size_t ESA_BINS = 32;
constexpr std::size_t HK_SIZE = 12;
constexpr double EPSILON = 1e-12;

inline std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

inline std::int16_t be16s(const std::uint8_t* p) {
    return static_cast<std::int16_t>(be16(p));
}

struct Decoder {
    std::uint64_t total = 0, fill = 0, checksum_pass = 0, checksum_fail = 0;
    std::uint8_t source_id = 0;
    std::string profile = "未知";

    std::vector<double> mag_t, mag_bt, mag_bx, mag_by, mag_bz, mag_v;
    std::vector<double> particle_t;
    std::array<std::vector<std::uint16_t>, 8> particle_ch;
    std::vector<double> esa_v_t, esa_g_t;
    std::vector<std::array<std::int16_t, ESA_BINS>> esa_v_bins, esa_g_bins;
    std::vector<double> hk_tsi_t, hk_tsi_v, ser_v_t, ser_v_v, ser_g_t, ser_g_v;

    void reserve(std::uint64_t frame_count) {
        const auto n = static_cast<std::size_t>(frame_count);
        const auto mag_n = n * MAG_PER_FRAME;
        for (auto* v : {&mag_t, &mag_bt, &mag_bx, &mag_by, &mag_bz, &mag_v}) v->reserve(mag_n);
        particle_t.reserve(n * PARTICLE_PER_FRAME);
        for (auto& v : particle_ch) v.reserve(n * PARTICLE_PER_FRAME);
        esa_v_t.reserve(n * 3); esa_v_bins.reserve(n * 3);
        esa_g_t.reserve(n * 3); esa_g_bins.reserve(n * 3);
        for (auto* v : {&hk_tsi_t, &hk_tsi_v, &ser_v_t, &ser_v_v, &ser_g_t, &ser_g_v}) v->reserve(n * 18);
    }

    static bool checksum_ok(const std::array<std::uint8_t, CADU_SIZE>& cadu) {
        std::uint32_t sum = 0;
        for (std::size_t i = 0; i < CADU_SIZE - 2; ++i) sum += cadu[i];
        return static_cast<std::uint16_t>(sum) == be16(cadu.data() + CADU_SIZE - 2);
    }

    void push(const std::array<std::uint8_t, CADU_SIZE>& cadu) {
        ++total;
        if (checksum_ok(cadu)) ++checksum_pass; else ++checksum_fail;

        const auto* p = cadu.data();
        const std::uint8_t ft = p[4];
        const std::uint16_t ct = be16(p + 10);
        const double t = ct * TIME_TICK_SECONDS + p[12] * FINE_TIME_STEP;
        if (ft == 0x77 || ft == 0x88) { ++fill; return; }

        if (source_id == 0) {
            source_id = p[9];
            profile = source_id == 0x30 ? "GGAK-E" :
                      (source_id == 0x31 || source_id == 0x33 ? "GGAK-VE" : "GGAK(未知)");
        }
        const auto* payload = p + PAYLOAD_OFFSET;
        switch (ft) {
        case 0x70: decode_mag(payload, t); break;
        case 0x40: decode_particle(payload, t); break;
        case 0x20: decode_esa(payload, t, 0x90, esa_v_t, esa_v_bins); break;
        case 0x30: decode_esa(payload, t, 0x98, esa_g_t, esa_g_bins); break;
        case 0x00: decode_hk(payload, t, 0x80); break;
        case 0x10: decode_ser(payload, t, 0x88); break;
        default: break;
        }
    }

    void decode_mag(const std::uint8_t* p, double t) {
        for (std::size_t i = 0; i < MAG_PER_FRAME; ++i) {
            const auto* q = p + i * MAG_LEN;
            const std::uint16_t x = be16(q + 2), y = be16(q + 4), z = be16(q + 6);
            if (x > 10000 || y > 10000 || z > 10000 || q[12] == 0xff) continue;
            mag_t.push_back(t);
            mag_bt.push_back((static_cast<int>(be16(q)) - 2339) * 2.359);
            mag_bx.push_back((static_cast<int>(x) - 2339) * 2.359);
            mag_by.push_back((static_cast<int>(y) - 2339) * 2.359);
            mag_bz.push_back((static_cast<int>(z) - 2339) * 2.359);
            mag_v.push_back(be16(q + 8) / 125.0);
        }
    }

    void decode_particle(const std::uint8_t* p, double t) {
        for (std::size_t i = 0; i < PARTICLE_PER_FRAME; ++i) {
            const auto* q = p + i * PARTICLE_LEN;
            if (q[0] != 0xa0 && q[0] != 0xa1) continue;
            bool fill_record = true;
            for (std::size_t j = 0; j < PARTICLE_LEN; ++j) fill_record &= q[j] == 0xaa;
            if (fill_record) continue;
            particle_t.push_back(t);
            for (std::size_t ch = 0; ch < 8; ++ch) particle_ch[ch].push_back(be16(q + 3 + ch * 2));
        }
    }

    void decode_esa(const std::uint8_t* p, double t, std::uint8_t marker,
                    std::vector<double>& times,
                    std::vector<std::array<std::int16_t, ESA_BINS>>& rows) {
        for (std::size_t off = 0; off <= 138; off += 69) {
            const auto* q = p + off;
            if (q[0] != marker) continue;
            bool fill_packet = true;
            for (std::size_t j = 0; j < ESA_SIZE; ++j) fill_packet &= q[j] == 0xaa;
            if (fill_packet) continue;
            std::array<std::int16_t, ESA_BINS> bins{};
            for (std::size_t ch = 0; ch < ESA_BINS; ++ch) bins[ch] = be16s(q + 3 + ch * 2);
            times.push_back(t); rows.push_back(bins);
        }
    }

    void decode_hk(const std::uint8_t* p, double t, std::uint8_t marker) {
        for (std::size_t off = 0; off + HK_SIZE <= PAYLOAD_SIZE;) {
            const auto* q = p + off;
            if (q[0] != marker) { ++off; continue; }
            bool fill_block = true;
            for (std::size_t j = 0; j < HK_SIZE; ++j) fill_block &= q[j] == 0xaa;
            if (!fill_block) { hk_tsi_t.push_back(t); hk_tsi_v.push_back(be16(q + 6) * 0.0331); }
            off += HK_SIZE;
        }
    }

    void decode_ser(const std::uint8_t* p, double t, std::uint8_t marker) {
        for (std::size_t off = 0; off + HK_SIZE <= PAYLOAD_SIZE;) {
            const auto* q = p + off;
            if (q[0] != marker) { ++off; continue; }
            bool fill_block = true;
            for (std::size_t j = 0; j < HK_SIZE; ++j) fill_block &= q[j] == 0xaa;
            if (!fill_block) {
                ser_v_t.push_back(t); ser_v_v.push_back(be16(q + 6));
                ser_g_t.push_back(t); ser_g_v.push_back(be16(q + 10));
            }
            off += HK_SIZE;
        }
    }
};

void print_summary(const Decoder& d) {
    std::cout << "\n==================================================\n"
              << "GGAK 解码概况\n==================================================\n"
              << "检测配置 : " << d.profile << " (Source ID: 0x"
              << std::hex << std::uppercase << static_cast<int>(d.source_id) << std::dec << ")\n"
              << "总帧数   : " << d.total << "\n有效帧   : " << d.total - d.fill
              << "\n填充帧   : " << d.fill << "\n校验通过 : " << d.checksum_pass
              << "\n校验失败 : " << d.checksum_fail << "\n磁强计数 : " << d.mag_t.size()
              << "\n粒子计数 : " << d.particle_t.size() << "\nESA-V 包 : " << d.esa_v_t.size()
              << "\nESA-G 包 : " << d.esa_g_t.size() << "\n平台服务 : " << d.hk_tsi_t.size()
              << "\nSER 计数 : " << d.ser_v_t.size() << "\n==================================================\n";
}

struct PlotArea { double x, y, w, h; };

std::pair<double, double> range_of(const std::vector<double>& values, double fallback_min = 0.0,
                                   double fallback_max = 1.0) {
    if (values.empty()) return {fallback_min, fallback_max};
    const auto [lo_it, hi_it] = std::minmax_element(values.begin(), values.end());
    double lo = *lo_it, hi = *hi_it;
    if (std::abs(hi - lo) < EPSILON) { lo -= 1.0; hi += 1.0; }
    const double pad = (hi - lo) * 0.05;
    return {lo - pad, hi + pad};
}

double relative_minutes(double time, double first_time) { return (time - first_time) / 60.0; }

double scale(double value, double from_lo, double from_hi, double to_lo, double to_hi) {
    return to_lo + (value - from_lo) * (to_hi - to_lo) / (from_hi - from_lo);
}

std::string color_for(std::size_t index) {
    static constexpr std::array<const char*, 8> colors = {
        "#16697a", "#d96c06", "#6a994e", "#9b2226", "#6c5ce7", "#0081a7", "#c1121f", "#6b705c"};
    return colors[index % colors.size()];
}

void svg_text(std::ostream& out, double x, double y, const std::string& text, int size = 13,
              const char* anchor = "start") {
    out << "<text x=\"" << x << "\" y=\"" << y << "\" font-size=\"" << size
        << "\" text-anchor=\"" << anchor << "\" fill=\"#17212b\">" << text << "</text>\n";
}

void plot_frame(std::ostream& out, const PlotArea& a, const std::string& title,
                double x0, double x1, double y0, double y1, bool x_label = false) {
    out << "<rect x=\"" << a.x << "\" y=\"" << a.y << "\" width=\"" << a.w << "\" height=\""
        << a.h << "\" fill=\"#ffffff\" stroke=\"#aeb8c2\"/>\n";
    svg_text(out, a.x, a.y - 8, title, 15);
    for (int i = 0; i <= 4; ++i) {
        const double x = a.x + a.w * i / 4.0, y = a.y + a.h * i / 4.0;
        out << "<path d=\"M " << x << ' ' << a.y << " V " << a.y + a.h << " M " << a.x << ' ' << y
            << " H " << a.x + a.w << "\" stroke=\"#e4e9ee\" stroke-width=\"1\"/>\n";
        std::ostringstream xs, ys;
        xs << std::fixed << std::setprecision(1) << (x0 + (x1 - x0) * i / 4.0);
        ys << std::fixed << std::setprecision(1) << (y1 - (y1 - y0) * i / 4.0);
        svg_text(out, x, a.y + a.h + 16, xs.str(), 10, "middle");
        svg_text(out, a.x - 6, y + 4, ys.str(), 10, "end");
    }
    if (x_label) svg_text(out, a.x + a.w / 2, a.y + a.h + 34, "relative time (min)", 11, "middle");
}

void plot_series(std::ostream& out, const PlotArea& a, const std::vector<double>& times,
                 const std::vector<double>& values, double first_time, double x0, double x1,
                 double y0, double y1, const std::string& color) {
    if (times.empty()) return;
    out << "<polyline fill=\"none\" stroke=\"" << color << "\" stroke-width=\"1.1\" points=\"";
    for (std::size_t i = 0; i < times.size(); ++i) {
        const double x = scale(relative_minutes(times[i], first_time), x0, x1, a.x, a.x + a.w);
        const double y = scale(values[i], y0, y1, a.y + a.h, a.y);
        out << x << ',' << y << ' ';
    }
    out << "\"/>\n";
}

void plot_legend(std::ostream& out, const PlotArea& a, const std::vector<std::string>& labels,
                 std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        const double x = a.x + 8 + (i % 4) * 92, y = a.y + 17 + (i / 4) * 14;
        out << "<line x1=\"" << x << "\" y1=\"" << y - 4 << "\" x2=\"" << x + 13
            << "\" y2=\"" << y - 4 << "\" stroke=\"" << color_for(i) << "\" stroke-width=\"2\"/>\n";
        svg_text(out, x + 17, y, labels[i], 10);
    }
}

std::string heat_color(double value) {
    const double v = std::clamp(value / 4.5, 0.0, 1.0);
    const int r = static_cast<int>(30 + 220 * v);
    const int g = static_cast<int>(40 + 175 * std::sin(v * 1.57079632679));
    const int b = static_cast<int>(105 + 100 * (1.0 - v));
    std::ostringstream s; s << "rgb(" << r << ',' << g << ',' << b << ')'; return s.str();
}

void plot_esa(std::ostream& out, const PlotArea& a, const std::string& title,
              const std::vector<double>& times,
              const std::vector<std::array<std::int16_t, ESA_BINS>>& bins) {
    if (times.empty()) { plot_frame(out, a, title, 0, 1, 0, 32); svg_text(out, a.x + a.w / 2, a.y + a.h / 2, "no data", 15, "middle"); return; }
    const double first = times.front();
    double x1 = relative_minutes(times.back(), first);
    if (x1 < EPSILON) x1 = 1.0;
    plot_frame(out, a, title, 0, x1, 0, 32);
    const double cell_w = a.w / bins.size(), cell_h = a.h / ESA_BINS;
    for (std::size_t col = 0; col < bins.size(); ++col) {
        for (std::size_t row = 0; row < ESA_BINS; ++row) {
            const double log_count = std::log10(std::max(0, static_cast<int>(bins[col][row])) + 1.0);
            out << "<rect x=\"" << a.x + col * cell_w << "\" y=\"" << a.y + row * cell_h
                << "\" width=\"" << std::max(1.0, cell_w) << "\" height=\"" << cell_h + 0.2
                << "\" fill=\"" << heat_color(log_count) << "\"/>\n";
        }
    }
}

bool write_svg(const Decoder& d, const std::string& output_path) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) return false;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1400\" height=\"1040\" viewBox=\"0 0 1400 1040\">\n"
        << "<rect width=\"1400\" height=\"1040\" fill=\"#f4f7f9\"/>\n";
    svg_text(out, 700, 34, "GGAK decoded instrument overview", 24, "middle");
    const std::array<PlotArea, 6> areas = {{{80, 85, 550, 230}, {760, 85, 550, 230},
                                               {80, 410, 550, 230}, {760, 410, 550, 230},
                                               {80, 735, 550, 230}, {760, 735, 550, 230}}};
    const std::vector<std::string> mag_labels = {"|B|", "Bx", "By", "Bz"};
    if (d.mag_t.empty()) { plot_frame(out, areas[0], "FM-VE magnetic field", 0, 1, 0, 1); svg_text(out, 355, 200, "no data", 15, "middle"); }
    else {
        const double first = d.mag_t.front(); double x1 = relative_minutes(d.mag_t.back(), first); if (x1 < EPSILON) x1 = 1.0;
        std::vector<double> all = d.mag_bt; all.insert(all.end(), d.mag_bx.begin(), d.mag_bx.end()); all.insert(all.end(), d.mag_by.begin(), d.mag_by.end()); all.insert(all.end(), d.mag_bz.begin(), d.mag_bz.end());
        const auto [y0, y1] = range_of(all); plot_frame(out, areas[0], "FM-VE magnetic field (nT)", 0, x1, y0, y1);
        plot_series(out, areas[0], d.mag_t, d.mag_bt, first, 0, x1, y0, y1, color_for(0)); plot_series(out, areas[0], d.mag_t, d.mag_bx, first, 0, x1, y0, y1, color_for(1)); plot_series(out, areas[0], d.mag_t, d.mag_by, first, 0, x1, y0, y1, color_for(2)); plot_series(out, areas[0], d.mag_t, d.mag_bz, first, 0, x1, y0, y1, color_for(3));
        plot_legend(out, areas[0], mag_labels, 4);
    }
    const std::vector<std::string> particle_labels = {"Ep600", "Ep800", "Ep1100", "Cg-1", "Cg-2", "Cg-3", "Cg-4", "MIP"};
    if (d.particle_t.empty()) { plot_frame(out, areas[1], "GALS-VE particle channels", 0, 1, 0, 1); svg_text(out, 1035, 200, "no data", 15, "middle"); }
    else {
        const double first = d.particle_t.front(); double x1 = relative_minutes(d.particle_t.back(), first); if (x1 < EPSILON) x1 = 1.0;
        std::vector<double> log_values; for (const auto& channel : d.particle_ch) for (auto v : channel) log_values.push_back(std::log10(std::max(1u, static_cast<unsigned>(v))));
        const auto [y0, y1] = range_of(log_values); plot_frame(out, areas[1], "GALS-VE particle channels (log10 count)", 0, x1, y0, y1);
        for (std::size_t ch = 0; ch < 8; ++ch) { std::vector<double> transformed; transformed.reserve(d.particle_ch[ch].size()); for (auto v : d.particle_ch[ch]) transformed.push_back(std::log10(std::max(1u, static_cast<unsigned>(v)))); plot_series(out, areas[1], d.particle_t, transformed, first, 0, x1, y0, y1, color_for(ch)); }
        plot_legend(out, areas[1], particle_labels, 8);
    }
    plot_esa(out, areas[2], "SKIF-VE/V ESA spectrum", d.esa_v_t, d.esa_v_bins);
    plot_esa(out, areas[3], "SKIF-VE/G ESA spectrum", d.esa_g_t, d.esa_g_bins);
    if (d.hk_tsi_t.empty()) { plot_frame(out, areas[4], "Platform ISP-2M TSI", 0, 1, 0, 1, true); svg_text(out, 355, 850, "no data", 15, "middle"); }
    else { const double first = d.hk_tsi_t.front(); double x1 = relative_minutes(d.hk_tsi_t.back(), first); if (x1 < EPSILON) x1 = 1.0; const auto [y0, y1] = range_of(d.hk_tsi_v); plot_frame(out, areas[4], "Platform ISP-2M TSI (W/m2)", 0, x1, y0, y1, true); plot_series(out, areas[4], d.hk_tsi_t, d.hk_tsi_v, first, 0, x1, y0, y1, color_for(0)); }
    if (d.ser_v_t.empty()) { plot_frame(out, areas[5], "SKIF-VE SER", 0, 1, 0, 1, true); svg_text(out, 1035, 850, "no data", 15, "middle"); }
    else { const double first = d.ser_v_t.front(); double x1 = relative_minutes(d.ser_v_t.back(), first); if (x1 < EPSILON) x1 = 1.0; std::vector<double> all = d.ser_v_v; all.insert(all.end(), d.ser_g_v.begin(), d.ser_g_v.end()); const auto [y0, y1] = range_of(all); plot_frame(out, areas[5], "SKIF-VE SER (counts/frame)", 0, x1, y0, y1, true); plot_series(out, areas[5], d.ser_v_t, d.ser_v_v, first, 0, x1, y0, y1, color_for(0)); plot_series(out, areas[5], d.ser_g_t, d.ser_g_v, first, 0, x1, y0, y1, color_for(1)); plot_legend(out, areas[5], {"VE/V", "VE/G"}, 2); }
    out << "</svg>\n";
    return static_cast<bool>(out);
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    if (argc < 2 || argc > 4 || (argc == 4 && std::string(argv[2]) != "-o" && std::string(argv[2]) != "--output")) { std::cerr << "用法: ggak_decode <input.cadu> [-o output.svg]\n"; return 2; }
    const std::string input_path = argv[1];
    const std::string output_path = argc == 4 ? argv[3] : input_path + ".svg";
    std::ifstream in(input_path, std::ios::binary);
    if (!in) { std::cerr << "无法打开输入文件: " << input_path << '\n'; return 1; }
    Decoder decoder;
    in.seekg(0, std::ios::end);
    const auto bytes = in.tellg();
    if (bytes > 0) decoder.reserve(static_cast<std::uint64_t>(bytes) / CADU_SIZE);
    in.seekg(0, std::ios::beg);
    std::array<std::uint8_t, CADU_SIZE> frame{};
    while (in.read(reinterpret_cast<char*>(frame.data()), frame.size())) decoder.push(frame);
    print_summary(decoder);
    if (decoder.total == 0) return 1;
    if (!write_svg(decoder, output_path)) { std::cerr << "无法写入图表: " << output_path << '\n'; return 1; }
    std::cout << "图表已保存至: " << output_path << '\n';
    return 0;
}
