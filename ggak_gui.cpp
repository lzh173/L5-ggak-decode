#include "imgui.h"
#include "ggak_core.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cmath>
#include <unordered_map>
#include <cstring>
#include <deque>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <atomic>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif

namespace {
using ggak::Data;
using ggak::EsaRow;
using ggak::ESA_BINS;
using ggak::FRAME;
using ggak::FrameDecodeState;
using ggak::be16;
using ggak::decode_file;
using ggak::decode_frame;
using ggak::valid;
constexpr std::array<uint8_t, 4> CADU_ASM = {0x1a, 0xcf, 0xfc, 0x1d};

#ifdef _WIN32
struct TcpSource {
    nng_socket socket{NNG_SOCKET_INITIALIZER};
    nng_dialer dialer{NNG_DIALER_INITIALIZER};
    bool opened = false;
    std::atomic<bool> pipe_connected{false};
    std::atomic<uint64_t> pipe_adds{0}, pipe_removes{0};
    uint64_t messages = 0, valid_size = 0, bad_size = 0, decode_frames = 0;
    uint64_t idle_messages = 0, idle_frames = 0, sync_words = 0, unsynced_bytes = 0;
    uint64_t checksum_pass = 0, checksum_fail = 0;
    size_t last_size = 0;
    std::string last_error = "未连接";
    std::string last_received = "无";
    std::string last_preview = "暂无消息";
    std::deque<std::string> recent;
    std::ofstream capture;
    std::string capture_path;
    uint64_t capture_bytes = 0;
    std::string host = "127.0.0.1";
    int port = 8888;
    bool connected = false;
    FrameDecodeState decoder;
};
#endif
#ifdef _WIN32
void on_pipe_event(nng_pipe, nng_pipe_ev event, void* context) {
    auto* tcp = static_cast<TcpSource*>(context);
    if(event == NNG_PIPE_EV_ADD_POST) { tcp->pipe_connected = true; ++tcp->pipe_adds; }
    else if(event == NNG_PIPE_EV_REM_POST) { tcp->pipe_connected = false; ++tcp->pipe_removes; }
}

std::string packet_preview(const uint8_t* bytes, size_t size) {
    std::ostringstream out;
    const size_t shown = std::min<size_t>(size, 64);
    for(size_t i = 0; i < shown; ++i) {
        if(i && i % 16 == 0) out << '\n';
        else if(i) out << ' ';
        out << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << unsigned(bytes[i]);
    }
    if(size > shown) out << "\n... (" << std::dec << size - shown << " bytes omitted)";
    return out.str();
}

bool is_fill_message(const uint8_t* bytes, size_t size) {
    return size != 0 && std::all_of(bytes, bytes + size, [](uint8_t b) { return b == 0x33; });
}

size_t count_asm(const uint8_t* bytes, size_t size) {
    size_t count = 0;
    for(size_t i = 0; i + CADU_ASM.size() <= size; ++i)
        if(std::equal(CADU_ASM.begin(), CADU_ASM.end(), bytes + i)) ++count;
    return count;
}

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    char text[32]; std::strftime(text, sizeof(text), "%H:%M:%S", &local);
    return text;
}

bool start_tcp(TcpSource& tcp, std::string& error) {
    if(tcp.connected) return true;
    tcp.decoder = FrameDecodeState{};
    const std::string url = "tcp://" + tcp.host + ":" + std::to_string(tcp.port);
    int result = nng_sub0_open_raw(&tcp.socket);
    if(result != 0) { error = std::string("NNG SUB 创建失败: ") + nng_strerror(result); return false; }
    nng_pipe_notify(tcp.socket, NNG_PIPE_EV_ADD_POST, on_pipe_event, &tcp);
    nng_pipe_notify(tcp.socket, NNG_PIPE_EV_REM_POST, on_pipe_event, &tcp);
    result = nng_dialer_create(&tcp.dialer, tcp.socket, url.c_str());
    if(result == 0) result = nng_dialer_start(tcp.dialer, NNG_FLAG_NONBLOCK);
    if(result != 0) { nng_close(tcp.socket); error = std::string("NNG 连接失败: ") + nng_strerror(result); return false; }
    tcp.opened = true; tcp.connected = true; tcp.last_error = "拨号器已启动，等待 NNG pipe"; return true;
}
void stop_tcp(TcpSource& tcp) {
    if(tcp.opened) nng_close(tcp.socket);
    if(tcp.capture.is_open()) tcp.capture.close();
    tcp.socket = nng_socket{NNG_SOCKET_INITIALIZER}; tcp.dialer = nng_dialer{NNG_DIALER_INITIALIZER};
    tcp.opened = false; tcp.connected = false; tcp.pipe_connected = false; tcp.last_error = "已手动断开";
}
void poll_tcp(TcpSource& tcp, bool strict, Data& data, std::string& error) {
    if(!tcp.connected) return;
    for(;;) {
        void* message = nullptr; size_t received = 0;
        const int result = nng_recv(tcp.socket, &message, &received, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);
        if(result == NNG_EAGAIN) break;
        if(result != 0) { error = std::string("NNG 接收失败: ") + nng_strerror(result); tcp.last_error = error; stop_tcp(tcp); return; }
        ++tcp.messages; tcp.last_size = received; tcp.last_received = timestamp_now();
        const std::string preview = packet_preview(static_cast<const uint8_t*>(message), received);
        tcp.recent.push_front(timestamp_now() + "  " + std::to_string(received) + " bytes");
        if(tcp.recent.size() > 12) tcp.recent.pop_back();
        tcp.last_preview = preview;
        const auto* bytes = static_cast<const uint8_t*>(message);
        if(tcp.capture.is_open()) {
            tcp.capture.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(received));
            if(tcp.capture) tcp.capture_bytes += received;
            else tcp.last_error = "CADU 保存写入失败";
        }
        if(is_fill_message(bytes, received)) {
            ++tcp.idle_messages;
            tcp.last_error = "收到 idle/fill 消息 (全 0x33)，无需解码";
            nng_free(message, received);
            continue;
        }
        ++tcp.valid_size;
        const size_t message_asm = count_asm(bytes, received);
        tcp.sync_words += message_asm;
        size_t frames_in_message = 0;
        size_t cursor = 0;
        while(cursor + FRAME <= received) {
            size_t sync = cursor;
            while(sync + CADU_ASM.size() <= received &&
                  !std::equal(CADU_ASM.begin(), CADU_ASM.end(), bytes + sync)) ++sync;
            if(sync + CADU_ASM.size() > received) break;
            if(sync > cursor) tcp.unsynced_bytes += sync - cursor;
            if(sync + FRAME > received) break;
            std::array<uint8_t, FRAME> frame{};
            std::copy_n(bytes + sync, FRAME, frame.begin());
            if(frame[4] == 0x77 || frame[4] == 0x88) ++tcp.idle_frames;
            ++tcp.decode_frames; ++frames_in_message;
            if(tcp.decoder.previous_coarse>=0 && tcp.decoder.previous_coarse-int(be16(frame.data()+10))>50000)
                tcp.decoder.coarse_offset += 65536;
            const uint64_t before_pass = data.pass, before_fail = data.fail;
            decode_frame(frame, strict, data, tcp.decoder);
            if(data.pass > before_pass) ++tcp.checksum_pass;
            if(data.fail > before_fail) ++tcp.checksum_fail;
            cursor = sync + FRAME;
        }
        if(frames_in_message == 0) {
            ++tcp.bad_size;
            tcp.last_error = message_asm ? "找到 ASM，但消息末尾不足完整 224-byte CADU" : "未找到 CADU ASM: 1A CF FC 1D";
        } else {
            tcp.last_error = "收到 NNG message，提取 " + std::to_string(frames_in_message) + " 帧";
        }
        nng_free(message, received);
    }
}
#endif
void plot_multi(const char* label, const char* id, const std::vector<std::vector<float>>& series,
                const std::vector<ImVec4>& colors, const std::vector<const char*>& names, bool log_scale = false) {
    ImGui::TextUnformatted(label);
    size_t count = 0; for(const auto& v : series) count = std::max(count, v.size());
    if(count == 0) { ImGui::TextDisabled("无数据"); return; }
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float chart_height = std::max(240.0f, available.y - 8.0f);
    const ImVec2 size(std::max(240.0f, available.x), chart_height);
    ImGui::InvisibleButton(id, size);
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float left = min.x + 48.0f, right = max.x - 10.0f;
    // Reserve space for the title, axis labels, and a wrapping legend footer.
    const float top = min.y + 30.0f;
    const float legend_width = std::max(1.0f, right - left);
    int legend_rows = 1;
    float legend_line_width = 0.0f;
    for(const char* name : names) {
        const float item_width = 28.0f + ImGui::CalcTextSize(name).x;
        if(legend_line_width > 0.0f && legend_line_width + item_width + 18.0f > legend_width) {
            ++legend_rows;
            legend_line_width = 0.0f;
        }
        legend_line_width += item_width + 18.0f;
    }
    const float bottom = max.y - (25.0f + 18.0f * legend_rows);
    const float plot_width = std::max(1.0f, right - left);
    struct View { float zoom_x = 1.0f, zoom_y = 1.0f; float center_x = 0.5f, center_y = 0.5f; };
    static std::unordered_map<std::string, View> views;
    View& view = views[id];
    if (ImGui::IsItemHovered()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (std::abs(io.MouseWheel) > 0.0f) {
            const float factor = io.MouseWheel > 0.0f ? 1.25f : 0.8f;
            const bool only_x = io.KeyCtrl && !io.KeyShift;
            const bool only_y = io.KeyShift && !io.KeyCtrl;
            const float mouse_x = std::clamp((io.MousePos.x - left) / plot_width, 0.0f, 1.0f);
            const float mouse_y = std::clamp((bottom - io.MousePos.y) / std::max(1.0f, bottom - top), 0.0f, 1.0f);
            if(!only_y) {
                const float old_zoom = view.zoom_x;
                view.zoom_x = std::clamp(view.zoom_x * factor, 1.0f, 256.0f);
                const float cursor_data = view.center_x - 0.5f / old_zoom + mouse_x / old_zoom;
                view.center_x = cursor_data - (mouse_x - 0.5f) / view.zoom_x;
            }
            if(!only_x) {
                const float old_zoom = view.zoom_y;
                view.zoom_y = std::clamp(view.zoom_y * factor, 1.0f, 256.0f);
                const float cursor_data = view.center_y - 0.5f / old_zoom + mouse_y / old_zoom;
                view.center_y = cursor_data - (mouse_y - 0.5f) / view.zoom_y;
            }
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            const bool only_x = io.KeyCtrl && !io.KeyShift;
            const bool only_y = io.KeyShift && !io.KeyCtrl;
            if(!only_y) view.center_x -= io.MouseDelta.x / plot_width / view.zoom_x;
            if(!only_x) view.center_y += io.MouseDelta.y / std::max(1.0f, bottom - top) / view.zoom_y;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) view = View{};
    }
    const float half_span_x = 0.5f / view.zoom_x;
    const float half_span_y = 0.5f / view.zoom_y;
    view.center_x = std::clamp(view.center_x, half_span_x, 1.0f - half_span_x);
    view.center_y = std::clamp(view.center_y, half_span_y, 1.0f - half_span_y);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(15, 15, 16, 255));
    draw->AddRect(min, max, IM_COL32(70, 74, 80, 255));

    std::vector<std::vector<float>> display = series;
    float lo = FLT_MAX, hi = -FLT_MAX;
    for(auto& values : display) for(float& value : values) {
        if(log_scale) value = std::log10(std::max(1.0f, value));
        lo = std::min(lo, value); hi = std::max(hi, value);
    }
    if(hi - lo < 1e-6f) hi = lo + 1.0f;
    const float width = std::max(1.0f, right - left);
    const float height = std::max(1.0f, bottom - top);
    constexpr int x_ticks = 10, y_ticks = 6;
    const float visible_start = std::clamp(view.center_x - half_span_x, 0.0f, 1.0f);
    const float visible_end = std::clamp(view.center_x + half_span_x, 0.0f, 1.0f);
    const float visible_y_start = std::clamp(view.center_y - half_span_y, 0.0f, 1.0f);
    const float visible_y_end = std::clamp(view.center_y + half_span_y, 0.0f, 1.0f);
    for(int i = 0; i <= x_ticks; ++i) {
        const float x = left + width * float(i) / x_ticks;
        draw->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(55, 57, 60, 180));
        const size_t sample = static_cast<size_t>(visible_start * count + (visible_end - visible_start) * count * float(i) / x_ticks);
        char tick[32]; std::snprintf(tick, sizeof(tick), "%zu", sample);
        draw->AddText(ImVec2(x - 10.0f, bottom + 7.0f), IM_COL32(220,220,220,255), tick);
    }
    for(int i = 0; i <= y_ticks; ++i) {
        const float y = bottom - height * float(i) / y_ticks;
        draw->AddLine(ImVec2(left, y), ImVec2(right, y), IM_COL32(55, 57, 60, 180));
        const float value = lo + (hi - lo) * (visible_y_start + (visible_y_end-visible_y_start) * float(i) / y_ticks);
        char tick[32]; std::snprintf(tick, sizeof(tick), "%.3g", value);
        draw->AddText(ImVec2(min.x + 5.0f, y - 8.0f), IM_COL32(220,220,220,255), tick);
    }
    draw->AddText(ImVec2((min.x + max.x) * .5f - ImGui::CalcTextSize(label).x * .5f, min.y + 6.0f), IM_COL32(240,240,240,255), label);
    const size_t columns = std::max<size_t>(2, static_cast<size_t>(width));
    draw->PushClipRect(ImVec2(left, top), ImVec2(right, bottom), true);
    for(size_t s = 0; s < display.size(); ++s) {
        const auto& v = display[s]; if(v.empty()) continue;
        const ImU32 line_color = ImGui::ColorConvertFloat4ToU32(colors[s]);
        if(v.size() < 3) {
            for(size_t i = 0; i < v.size(); ++i) {
                const float ratio = v.size() == 1 ? 0.5f : float(i) / float(v.size() - 1);
                if(ratio < visible_start || ratio > visible_end) continue;
                const float x = left + width * (ratio - visible_start) / std::max(1e-6f, visible_end - visible_start);
                const float y = bottom - height * ((v[i] - lo) / (hi - lo) - visible_y_start) / std::max(1e-6f, visible_y_end-visible_y_start);
                draw->AddCircleFilled(ImVec2(x, y), 3.5f, line_color);
            }
            continue;
        }
        const float sample_start = std::clamp(visible_start * float(v.size() - 1), 0.0f, float(v.size() - 1));
        const float sample_end = std::clamp(visible_end * float(v.size() - 1), sample_start, float(v.size() - 1));
        const size_t first = static_cast<size_t>(sample_start);
        const size_t last = static_cast<size_t>(std::ceil(sample_end));
        const size_t point_count = last >= first ? last - first + 1 : 0;
        if(point_count <= columns) {
            ImVec2 previous{}; bool have_previous = false;
            for(size_t i = first; i <= last && i < v.size(); ++i) {
                const float ratio = float(i) / float(v.size() - 1);
                const float x = left + width * (ratio - visible_start) / std::max(1e-6f, visible_end - visible_start);
                const float y = bottom - height * ((v[i] - lo) / (hi - lo) - visible_y_start) / std::max(1e-6f, visible_y_end-visible_y_start);
                const ImVec2 point(x, y);
                if(have_previous) draw->AddLine(previous, point, line_color, 1.5f);
                else draw->AddCircleFilled(point, 2.0f, line_color);
                previous = point; have_previous = true;
            }
            continue;
        }
        for(size_t column = 0; column < columns; ++column) {
            const size_t begin = std::min(v.size() - 1, static_cast<size_t>(sample_start + (sample_end - sample_start) * float(column) / float(columns)));
            const size_t end = std::max(begin + 1, std::min(v.size(), static_cast<size_t>(sample_start + (sample_end - sample_start) * float(column + 1) / float(columns))));
            float column_lo = v[begin], column_hi = v[begin];
            for(size_t i = begin; i < std::min(end, v.size()); ++i) { column_lo = std::min(column_lo, v[i]); column_hi = std::max(column_hi, v[i]); }
            const float x = left + width * static_cast<float>(column) / static_cast<float>(columns - 1);
            const float y_lo = bottom - height * ((column_lo - lo) / (hi - lo) - visible_y_start) / std::max(1e-6f, visible_y_end-visible_y_start);
            const float y_hi = bottom - height * ((column_hi - lo) / (hi - lo) - visible_y_start) / std::max(1e-6f, visible_y_end-visible_y_start);
            draw->AddLine(ImVec2(x, y_lo), ImVec2(x, y_hi), line_color, 1.0f);
        }
    }
    draw->PopClipRect();
    // Draw the legend below the axes so it can never cover the data lines.
    float legend_x = left;
    float legend_y = bottom + 25.0f;
    for(size_t s=0;s<display.size();++s) {
        const float item_width = 28.0f + ImGui::CalcTextSize(names[s]).x;
        if(legend_x + item_width + 18.0f > right && legend_x > left) {
            legend_x = left;
            legend_y += 18.0f;
        }
        draw->AddRectFilled(ImVec2(legend_x, legend_y + 5.0f),
                            ImVec2(legend_x + 14.0f, legend_y + 8.0f),
                            ImGui::ColorConvertFloat4ToU32(colors[s]));
        draw->AddText(ImVec2(legend_x + 19.0f, legend_y), IM_COL32(220,225,235,230), names[s]);
        legend_x += item_width + 18.0f;
    }
    char range[128]; std::snprintf(range, sizeof(range), "%s  min %.3g   max %.3g   n=%zu  Xx%.1f Yx%.1f", log_scale ? "log10" : "", lo, hi, count, view.zoom_x, view.zoom_y);
    draw->AddText(ImVec2(right - 250.0f, top + 5.0f), IM_COL32(210, 220, 235, 220), range);
    ImGui::TextDisabled("有效点: %zu", count);
}

void plot(const char* label, const char* id, const std::vector<float>& v, ImVec4 color) { plot_multi(label,id,{v},{color},{"data"}); }

void plot_spectral(const std::vector<std::vector<float>>& packets) {
    std::array<std::vector<float>, 8> channels;
    for(const auto& packet : packets)
        for(size_t i=0; i<std::min<size_t>(channels.size(), packet.size()); ++i)
            channels[i].push_back(packet[i]);
    std::vector<std::vector<float>> series;
    std::vector<const char*> names;
    const std::array<const char*,8> labels = {"CH1","CH2","CH3","CH4","CH5","CH6","CH7","CH8"};
    const std::array<ImVec4,8> colors = {ImVec4(1,.55f,.25f,1),ImVec4(.2f,.75f,1,1),ImVec4(.3f,1,.45f,1),ImVec4(1,.35f,.45f,1),ImVec4(.8f,.5f,1,1),ImVec4(1,.85f,.3f,1),ImVec4(.5f,1,.8f,1),ImVec4(1,.6f,.7f,1)};
    for(size_t i=0;i<channels.size();++i) if(!channels[i].empty()) { series.push_back(channels[i]); names.push_back(labels[i]); }
    std::vector<ImVec4> used_colors;
    for(size_t i=0;i<series.size();++i) used_colors.push_back(colors[i]);
    plot_multi("SKL-E 光谱", "##plot_skl_spectral", series, used_colors, names);
}

void metric(const char* label, unsigned long long value) {
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine();
    ImGui::Text("%llu", value);
}

void heatmap(const char* label, const char* id, const std::vector<EsaRow>& rows) {
    ImGui::TextUnformatted(label);
    if(rows.empty()) { ImGui::TextDisabled("无数据"); return; }
    const ImVec2 size(ImGui::GetContentRegionAvail().x, 230.0f);
    ImGui::InvisibleButton(id, size);
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(18, 30, 48, 255));
    draw->AddRect(min, max, IM_COL32(63, 91, 125, 255));
    int16_t lo = rows.front()[0], hi = rows.front()[0];
    for(const auto& row : rows) for(int16_t value : row) { lo = std::min(lo, value); hi = std::max(hi, value); }
    if(hi == lo) hi = static_cast<int16_t>(lo + 1);
    const float cell_w = (max.x - min.x - 12.0f) / float(ESA_BINS);
    const float cell_h = (max.y - min.y - 12.0f) / float(rows.size());
    for(size_t r = 0; r < rows.size(); ++r) for(size_t c = 0; c < ESA_BINS; ++c) {
        const float ratio = float(rows[r][c] - lo) / float(hi - lo);
        const ImU32 color = ImColor::HSV(0.64f - 0.64f * std::clamp(ratio, 0.0f, 1.0f), 0.85f, 0.25f + 0.75f * ratio);
        const ImVec2 a(min.x + 6.0f + c * cell_w, min.y + 6.0f + r * cell_h);
        draw->AddRectFilled(a, ImVec2(a.x + cell_w + .5f, a.y + cell_h + .5f), color);
    }
    char range[96]; std::snprintf(range, sizeof(range), "min %d   max %d   包 %zu", int(lo), int(hi), rows.size());
    draw->AddText(ImVec2(min.x + 8.0f, min.y + 5.0f), IM_COL32(235, 240, 250, 230), range);
    constexpr double energies[12] = {0.05,0.25,0.45,0.80,1.30,2.50,4.50,7.50,10.00,13.00,16.00,20.00};
    const float energy_step = (max.x-min.x-12.0f) / 12.0f;
    for(size_t i=0;i<12;i++) {
        char text[24]; std::snprintf(text,sizeof(text),"%.2g",energies[i]);
        draw->AddText(ImVec2(min.x+6.0f+energy_step*(float(i)+0.5f)-10.0f,max.y-17.0f),IM_COL32(225,230,240,220),text);
    }
    draw->AddText(ImVec2(max.x-70.0f,max.y-17.0f),IM_COL32(190,200,215,220),"keV");
}

void load_chinese_font(ImGuiIO& io) {
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    const std::array<const char*, 4> candidates = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc"
    };
    for (const char* path : candidates) {
        if (!std::filesystem::is_regular_file(path)) continue;
        ImFont* font = io.Fonts->AddFontFromFileTTF(path, 18.0f, nullptr, ranges);
        if (font) { io.FontDefault = font; return; }
    }
    io.FontDefault = io.Fonts->AddFontDefault();
}

bool choose_cadu_file(char* path, size_t path_size) {
#ifdef _WIN32
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(path_size);
    dialog.lpstrFilter = "GGAK CADU files (*.cadu)\0*.cadu\0All files (*.*)\0*.*\0\0";
    dialog.nFilterIndex = 1;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&dialog) == TRUE;
#else
    (void)path; (void)path_size;
    return false;
#endif
}
}

int main() {
    if(!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR,3); glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR,3);
    GLFWwindow* window=glfwCreateWindow(1280,820,"GGAK CADU 图形化解码器",nullptr,nullptr); if(!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window); glfwSwapInterval(1); IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO& io=ImGui::GetIO(); load_chinese_font(io);
    ImGui::StyleColorsDark(); ImGui_ImplGlfw_InitForOpenGL(window,true); ImGui_ImplOpenGL3_Init("#version 330");
    char path[1024]=""; bool strict=false; Data data; std::string status="请输入 CADU 文件路径";
#ifdef _WIN32
    TcpSource tcp;
    char tcp_host[128] = "127.0.0.1";
    int tcp_port = 8888;
    bool tcp_live = false;
    bool save_capture = true;
    char capture_path[1024] = "received_live.cadu";
#endif
    while(!glfwWindowShouldClose(window)) {
        glfwPollEvents();
#ifdef _WIN32
        if(tcp_live) poll_tcp(tcp, strict, data, status);
#endif
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0),ImGuiCond_Always); ImGui::SetNextWindowSize(io.DisplaySize,ImGuiCond_Always);
        ImGui::Begin("GGAK 解码工作台",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
    ImGui::Text("GGAK CADU 解码器"); ImGui::SameLine(); ImGui::TextDisabled("%s",status.c_str()); ImGui::Separator();
        ImGui::BeginChild("sidebar",ImVec2(255,0),true);
        ImGui::Text("文件"); ImGui::Separator();
        ImGui::TextWrapped("%s", path[0] ? path : "未选择 CADU 文件");
        if(ImGui::Button("打开文件...##open_file",ImVec2(-1,30))) choose_cadu_file(path,sizeof(path));
        if(ImGui::Button("加载并解码##load",ImVec2(-1,30))) { data=Data{}; if(decode_file(path,strict,data,status)) status="解码完成"; }
        ImGui::Checkbox("严格校验##strict",&strict);
#ifdef _WIN32
        ImGui::Spacing(); ImGui::Text("TCP 实时输入"); ImGui::Separator();
        ImGui::InputText("地址##tcp_host", tcp_host, sizeof(tcp_host));
        ImGui::InputInt("端口##tcp_port", &tcp_port); tcp_port = std::clamp(tcp_port, 1, 65535);
        ImGui::Checkbox("保存接收 CADU##save_capture", &save_capture);
        if(save_capture) ImGui::InputText("保存到##capture_path", capture_path, sizeof(capture_path));
        if(!tcp_live) {
            if(ImGui::Button("连接 TCP##tcp_connect",ImVec2(-1,30))) {
                tcp.host = tcp_host; tcp.port = tcp_port;
                if(save_capture) {
                    tcp.capture_path = capture_path;
                    tcp.capture.open(tcp.capture_path, std::ios::binary | std::ios::trunc);
                    if(!tcp.capture) { status = "无法创建 CADU 保存文件"; }
                    else tcp.capture_bytes = 0;
                }
                if((!save_capture || tcp.capture.is_open()) && start_tcp(tcp,status)) {
                    tcp_live=true;
                    status = data.total ? "NNG 已启动，正在当前 CADU 数据后接续解码" : "NNG 已启动，等待 CADU 数据";
                }
                else if(tcp.capture.is_open()) tcp.capture.close();
            }
        } else if(ImGui::Button("断开 TCP##tcp_disconnect",ImVec2(-1,30))) {
            stop_tcp(tcp); tcp_live=false; status="网络输入已断开，CADU 文件已关闭";
        }
        if(tcp_live && save_capture) ImGui::TextWrapped("保存中: %s (%llu bytes)", tcp.capture_path.c_str(), (unsigned long long)tcp.capture_bytes);
        if(data.total && !tcp_live) ImGui::TextDisabled("当前已有 CADU 数据；连接 TCP 后将继续追加实时数据");
        ImGui::TextDisabled("NNG message: pkt_size 1792 bytes / CADU 224 bytes");
#endif
        ImGui::Spacing(); ImGui::Text("操作说明"); ImGui::Separator();
        ImGui::TextWrapped("左键拖动：平移图表");
        ImGui::TextWrapped("滚轮：同时缩放 X/Y 轴");
        ImGui::TextWrapped("Ctrl + 滚轮/拖动：仅操作 X 轴");
        ImGui::TextWrapped("Shift + 滚轮/拖动：仅操作 Y 轴");
        ImGui::TextWrapped("双击左键：恢复默认视图");
        ImGui::TextDisabled("Ctrl + 右键：当前未分配操作");
        ImGui::Spacing(); ImGui::Text("数据类型"); ImGui::Separator();
        ImGui::BulletText("FM-VE 磁场"); ImGui::BulletText("GALS-VE 粒子");
        ImGui::BulletText("SKIF-VE/V ESA"); ImGui::BulletText("SKIF-VE/G ESA");
        ImGui::BulletText("ISP-2M / SER / SKL-E 光谱");
        if(data.total) { ImGui::Spacing(); ImGui::Text("状态"); ImGui::Separator(); ImGui::Text("配置: %s",data.profile.c_str()); ImGui::Text("Source ID: 0x%02X",data.source); ImGui::Text("总帧: %llu",(unsigned long long)data.total); }
        ImGui::EndChild(); ImGui::SameLine();
        ImGui::BeginChild("workspace",ImVec2(0,0),false);
        if(ImGui::BeginTabBar("main_tabs")) {
            if(ImGui::BeginTabItem("概览##overview_tab")) {
                if(!data.total) { ImGui::TextDisabled("尚未加载 CADU 文件"); }
                else {
                    ImGui::Text("配置: %s    Source ID: 0x%02X",data.profile.c_str(),data.source);
                    ImGui::Spacing();
                    if(ImGui::BeginTable("metrics",4,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)) {
                        ImGui::TableNextColumn(); metric("总帧",(unsigned long long)data.total);
                        ImGui::TableNextColumn(); metric("校验通过",(unsigned long long)data.pass);
                        ImGui::TableNextColumn(); metric("校验失败",(unsigned long long)data.fail);
                        ImGui::TableNextColumn(); metric("填充帧",(unsigned long long)data.fill);
                        ImGui::EndTable();
                    }
                    ImGui::Spacing();
                    ImGui::Text("数据点概览");
                    ImGui::BulletText("FM-VE 磁场: %d", (int)data.mag[0].size());
                    ImGui::BulletText("GALS-VE 粒子: %d", (int)data.particle[0].size());
                    ImGui::BulletText("SKIF-VE/V ESA: %d 包", (int)data.esa_v.size());
                    ImGui::BulletText("SKIF-VE/G ESA: %d 包", (int)data.esa_g.size());
                    ImGui::BulletText("SKL-E 光谱: %llu 包", (unsigned long long)data.spectral_packets);
                    ImGui::BulletText("Housekeeping-B/N2: %llu 块", (unsigned long long)data.housekeeping_b_count);
                    ImGui::BulletText("ISP-2M TSI: %d", (int)data.tsi.size());
                    ImGui::BulletText("SKIF-VE SER: %d", (int)data.ser.size());
                }
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("图表##charts_tab")) {
                if(!data.total) ImGui::TextDisabled("尚未加载 CADU 文件");
                else if(ImGui::BeginTabBar("instrument_tabs")) {
                    if(ImGui::BeginTabItem("FM-VE 磁场##instrument_fm")) {
                        plot_multi("FM-VE 磁场","##plot_mag",{data.mag[0],data.mag[1],data.mag[2],data.mag[3],data.mag_voltage},
                            {ImVec4(.95f,.95f,.95f,1),ImVec4(.2f,.75f,1,1),ImVec4(.3f,1,.45f,1),ImVec4(1,.55f,.25f,1),ImVec4(1,.3f,.8f,1)},
                            {"|B|","Bx","By","Bz","电压"});
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("GALS-VE 粒子##instrument_gals")) {
                        plot_multi("GALS-VE 粒子计数","##plot_particle",
                            {data.particle[0],data.particle[1],data.particle[2],data.particle[3],data.particle[4],data.particle[5],data.particle[6],data.particle[7]},
                            {ImVec4(1,.65f,.2f,1),ImVec4(.2f,.75f,1,1),ImVec4(.3f,1,.45f,1),ImVec4(1,.35f,.45f,1),ImVec4(.8f,.5f,1,1),ImVec4(1,.85f,.3f,1),ImVec4(.5f,1,.8f,1),ImVec4(1,.6f,.7f,1)},
                            {"Ep>=600MeV","Ep>=800MeV","Ep>=1100MeV","Cg-1","Cg-2","Cg-3","Cg-4","MIP"}, true);
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("SKIF-VE/V##instrument_esa_v")) {
                        heatmap("SKIF-VE/V ESA 能谱","##heatmap_esa_v",data.esa_v);
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("SKIF-VE/G##instrument_esa_g")) {
                        heatmap("SKIF-VE/G ESA 能谱","##heatmap_esa_g",data.esa_g);
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("ISP-2M##instrument_tsi")) {
                        plot("ISP-2M TSI","##plot_tsi",data.tsi,ImVec4(.3f,1,.45f,1));
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("SER##instrument_ser")) {
                        plot("SKIF-VE SER","##plot_ser",data.ser,ImVec4(1,.35f,.45f,1));
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("Housekeeping##instrument_hk")) {
                        plot_multi("Housekeeping-A 通道","##plot_hk_a",
                            {data.housekeeping_a[0],data.housekeeping_a[1],data.housekeeping_a[2],data.housekeeping_a[3],data.housekeeping_a[4]},
                            {ImVec4(1,.55f,.25f,1),ImVec4(.2f,.75f,1,1),ImVec4(.3f,1,.45f,1),ImVec4(1,.35f,.45f,1),ImVec4(.8f,.5f,1,1)},
                            {"CH1","CH2","CH3","CH4","CH5"});
                        ImGui::Separator();
                        plot_multi("Housekeeping-B / N2 通道","##plot_hk_b",
                            {data.housekeeping_b[0],data.housekeeping_b[1],data.housekeeping_b[2],data.housekeeping_b[3],data.housekeeping_b[4]},
                            {ImVec4(1,.55f,.25f,1),ImVec4(.2f,.75f,1,1),ImVec4(.3f,1,.45f,1),ImVec4(1,.35f,.45f,1),ImVec4(.8f,.5f,1,1)},
                            {"CH1","CH2","CH3","CH4","CH5"});
                        ImGui::EndTabItem();
                    }
                    if(ImGui::BeginTabItem("SKL-E##instrument_skl")) {
                        if(data.skl_spectral.empty()) ImGui::TextDisabled("暂无 SKL-E 光谱包");
                        else plot_spectral(data.skl_spectral);
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("数据##data_tab")) {
                ImGui::TextWrapped("当前解码结果");
                ImGui::Separator();
                ImGui::Text("FM-VE: %d 个点",(int)data.mag[0].size());
                ImGui::Text("GALS-VE: %d 个点",(int)data.particle[0].size());
                ImGui::Text("SKIF-VE/V ESA: %d 个包",(int)data.esa_v.size());
                ImGui::Text("SKIF-VE/G ESA: %d 个包",(int)data.esa_g.size());
                ImGui::Text("ISP-2M: %d 个点",(int)data.tsi.size());
                ImGui::Text("SER: %d 个点",(int)data.ser.size());
                ImGui::Text("SKL-E 光谱: %llu 个包",(unsigned long long)data.spectral_packets);
                ImGui::Text("Housekeeping-A/B: %d / %llu 个块",(int)data.housekeeping_a[0].size(),(unsigned long long)data.housekeeping_b_count);
                ImGui::Text("计数器跳号: %llu 次，估计丢帧: %llu",(unsigned long long)data.frame_gaps,(unsigned long long)data.missing_frames);
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("仪器目录##instrument_catalog_tab")) {
                ImGui::TextUnformatted("GGAK-E 仪器目录");
                ImGui::Separator();
                if(ImGui::BeginTable("instrument_catalog", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("仪器");
                    ImGui::TableSetupColumn("说明");
                    ImGui::TableSetupColumn("协议状态");
                    ImGui::TableSetupColumn("当前数据");
                    ImGui::TableHeadersRow();
                    const char* rows[][4] = {
                        {"GGAK-E/SKIF-6", "粒子辐射谱仪", "已接入", "0x20 / 0x30 ESA"},
                        {"GGAK-E/GALS-E", "银河宇宙线探测器", "已接入", "0x40 粒子计数"},
                        {"GGAK-E/ISP-2M", "太阳常数传感器", "已接入", "0x00 TSI"},
                        {"GGAK-E/VUSS-E", "太阳紫外辐射传感器", "待接入", "尚无对应解码分支"},
                        {"GGAK-E/FM-E", "磁强计", "已接入", "0x70 磁场/电压"},
                        {"GGAK-E/DIR-E", "太阳 X 射线辐射通量传感器", "待接入", "尚无对应解码分支"},
                        {"GGAK-E/SKL-E", "太阳宇宙线谱仪", "已接入", "0x50 / 0x5C 光谱"},
                    };
                    for(const auto& row : rows) {
                        ImGui::TableNextRow();
                        for(int col=0; col<4; ++col) { ImGui::TableSetColumnIndex(col); ImGui::TextWrapped("%s", row[col]); }
                    }
                    ImGui::EndTable();
                }
                ImGui::Spacing();
                ImGui::TextDisabled("VUSS-E 和 DIR-E 已列入仪器目录，但 SatDump PR #1143 的 GGAK reader 未提供对应帧类型解析，因此当前不会伪造数据曲线。");
                ImGui::EndTabItem();
            }
#ifdef _WIN32
            if(ImGui::BeginTabItem("网络调试##network_debug_tab")) {
                ImGui::Text("NNG raw SUB"); ImGui::SameLine();
                if(tcp.pipe_connected.load()) ImGui::TextColored(ImVec4(.35f,1,.45f,1), "已建立 pipe");
                else if(tcp_live) ImGui::TextColored(ImVec4(1,.75f,.25f,1), "拨号中 / 等待发布端");
                else ImGui::TextDisabled("未连接");
                ImGui::Text("目标: tcp://%s:%d", tcp.host.c_str(), tcp.port);
                ImGui::TextWrapped("状态: %s", tcp.last_error.c_str());
                ImGui::Separator();
                if(ImGui::BeginTable("network_counters", 4, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableNextColumn(); ImGui::Text("NNG pipe 建立: %llu", (unsigned long long)tcp.pipe_adds.load());
                    ImGui::TableNextColumn(); ImGui::Text("NNG pipe 断开: %llu", (unsigned long long)tcp.pipe_removes.load());
                    ImGui::TableNextColumn(); ImGui::Text("收到消息: %llu", (unsigned long long)tcp.messages);
                    ImGui::TableNextColumn(); ImGui::Text("最近消息长度: %zu", tcp.last_size);
                    ImGui::TableNextColumn(); ImGui::Text("非 idle 消息: %llu", (unsigned long long)tcp.valid_size);
                    ImGui::TableNextColumn(); ImGui::Text("全 0x33 idle: %llu", (unsigned long long)tcp.idle_messages);
                    ImGui::TableNextColumn(); ImGui::Text("idle CADU(77/88): %llu", (unsigned long long)tcp.idle_frames);
                    ImGui::TableNextColumn(); ImGui::Text("找到 ASM: %llu", (unsigned long long)tcp.sync_words);
                    ImGui::TableNextColumn(); ImGui::Text("提取 CADU: %llu", (unsigned long long)tcp.decode_frames);
                    ImGui::TableNextColumn(); ImGui::Text("无完整帧消息: %llu", (unsigned long long)tcp.bad_size);
                    ImGui::TableNextColumn(); ImGui::Text("未同步字节: %llu", (unsigned long long)tcp.unsynced_bytes);
                    ImGui::TableNextColumn(); ImGui::Text("校验通过/失败: %llu / %llu", (unsigned long long)tcp.checksum_pass, (unsigned long long)tcp.checksum_fail);
                    ImGui::TableNextColumn(); ImGui::Text("帧计数跳号: %llu", (unsigned long long)data.frame_gaps);
                    ImGui::TableNextColumn(); ImGui::Text("估计丢帧: %llu", (unsigned long long)data.missing_frames);
                    ImGui::EndTable();
                }
                ImGui::Text("最近收包: %s", tcp.last_received.c_str());
                ImGui::TextWrapped("CADU 文件: %s", tcp.capture_path.empty() ? "未设置" : tcp.capture_path.c_str());
                ImGui::Text("已保存字节: %llu", (unsigned long long)tcp.capture_bytes);
                ImGui::Separator(); ImGui::Text("帧类型统计");
                ImGui::Text("00 HK-A %llu | 10 SER %llu | 20 ESA-V %llu | 30 ESA-G %llu",
                    (unsigned long long)data.frame_types[0x00],(unsigned long long)data.frame_types[0x10],
                    (unsigned long long)data.frame_types[0x20],(unsigned long long)data.frame_types[0x30]);
                ImGui::Text("40 GALS %llu | 50 SKL-cal %llu | 5C SKL-spec %llu | 60 HK-B %llu | 70 FM %llu",
                    (unsigned long long)data.frame_types[0x40],(unsigned long long)data.frame_types[0x50],
                    (unsigned long long)data.frame_types[0x5c],(unsigned long long)data.frame_types[0x60],
                    (unsigned long long)data.frame_types[0x70]);
                ImGui::Separator(); ImGui::Text("最近 NNG 消息 (hex, 最多显示前 64 bytes)");
                ImGui::BeginChild("packet_preview", ImVec2(0, 155), true);
                ImGui::TextUnformatted(tcp.last_preview.c_str());
                ImGui::EndChild();
                ImGui::Text("消息记录");
                ImGui::BeginChild("packet_history", ImVec2(0, 0), true);
                for(const auto& item : tcp.recent) ImGui::BulletText("%s", item.c_str());
                if(tcp.recent.empty()) ImGui::TextDisabled("尚未收到 NNG 消息");
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
#endif
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::End(); ImGui::Render(); int w,h; glfwGetFramebufferSize(window,&w,&h); glViewport(0,0,w,h); glClearColor(.055f,.065f,.08f,1); glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glfwSwapBuffers(window);
    }
    ImGui_ImplOpenGL3_Shutdown(); ImGui_ImplGlfw_Shutdown(); ImGui::DestroyContext(); glfwDestroyWindow(window); glfwTerminate();
#ifdef _WIN32
    stop_tcp(tcp);
#endif
    return 0;
}
