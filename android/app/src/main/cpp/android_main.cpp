#include "ggak_core.h"
#include "imgui.h"
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_opengl3.h"

#include <android/log.h>
#include <android/api-level.h>
#include <android/asset_manager.h>
#include <android/configuration.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <algorithm>
#include <cfloat>
#include <mutex>
#include <string>
#include <unistd.h>
#include <nng/nng.h>
#include <nng/protocol/pubsub0/sub.h>
#include <array>
#include <atomic>
#include <cmath>
#include <deque>
#include <sstream>
#include <unordered_map>
#include <cstdio>

namespace {
constexpr const char* LOG_TAG = "GGAKDecoder";
constexpr std::array<uint8_t, 4> CADU_ASM = {0x1a, 0xcf, 0xfc, 0x1d};
android_app* g_app = nullptr;
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
std::mutex g_file_mutex;
std::string g_pending_path, g_pending_name;
ggak::Data g_data;
std::string g_status = "请选择 CADU 文件";
bool g_strict = false;
ImVector<ImWchar> g_font_ranges;
float g_ui_scale = 1.0f;

struct TcpSource {
    nng_socket socket{NNG_SOCKET_INITIALIZER};
    nng_dialer dialer{NNG_DIALER_INITIALIZER};
    bool connected = false;
    bool pipe_connected = false;
    uint64_t pipe_adds = 0, pipe_removes = 0;
    uint64_t messages = 0, valid_messages = 0, bad_messages = 0, decode_frames = 0;
    uint64_t idle_messages = 0, idle_frames = 0, sync_words = 0, unsynced_bytes = 0;
    uint64_t checksum_pass = 0, checksum_fail = 0;
    size_t last_size = 0;
    std::string last_preview = "暂无消息";
    std::string host = "127.0.0.1";
    int port = 8888;
    std::string status = "未连接";
    ggak::FrameDecodeState decoder;
} g_tcp;

void on_pipe_event(nng_pipe, nng_pipe_ev event, void*) {
    if (event == NNG_PIPE_EV_ADD_POST) {
        g_tcp.pipe_connected = true;
        ++g_tcp.pipe_adds;
    } else if (event == NNG_PIPE_EV_REM_POST) {
        g_tcp.pipe_connected = false;
        ++g_tcp.pipe_removes;
    }
}

size_t count_asm(const uint8_t* bytes, size_t size) {
    size_t count = 0;
    for (size_t i = 0; i + CADU_ASM.size() <= size; ++i)
        if (std::equal(CADU_ASM.begin(), CADU_ASM.end(), bytes + i)) ++count;
    return count;
}

std::string packet_preview(const uint8_t* bytes, size_t size) {
    std::ostringstream out;
    const size_t shown = std::min<size_t>(size, 32);
    for (size_t i = 0; i < shown; ++i) {
        if (i) out << ' ';
        char text[4]; std::snprintf(text, sizeof(text), "%02X", bytes[i]); out << text;
    }
    if (size > shown) out << " ...";
    return out.str();
}

bool start_tcp() {
    if (g_tcp.connected) return true;
    const std::string url = "tcp://" + g_tcp.host + ":" + std::to_string(g_tcp.port);
    int result = nng_sub0_open_raw(&g_tcp.socket);
    if (!result) {
        nng_pipe_notify(g_tcp.socket, NNG_PIPE_EV_ADD_POST, on_pipe_event, nullptr);
        nng_pipe_notify(g_tcp.socket, NNG_PIPE_EV_REM_POST, on_pipe_event, nullptr);
        result = nng_dialer_create(&g_tcp.dialer, g_tcp.socket, url.c_str());
    }
    if (!result) result = nng_dialer_start(g_tcp.dialer, NNG_FLAG_NONBLOCK);
    if (result) { g_tcp.status = std::string("NNG 错误: ") + nng_strerror(result); return false; }
    g_tcp.connected = true; g_tcp.pipe_connected = false;
    g_tcp.status = "NNG 已启动，等待服务端"; g_tcp.decoder = {};
    return true;
}

void stop_tcp() {
    if (g_tcp.connected) nng_close(g_tcp.socket);
    g_tcp.socket = nng_socket{NNG_SOCKET_INITIALIZER};
    g_tcp.dialer = nng_dialer{NNG_DIALER_INITIALIZER};
    g_tcp.connected = false; g_tcp.pipe_connected = false; g_tcp.status = "未连接";
}

void poll_tcp() {
    if (!g_tcp.connected) return;
    for (;;) {
        void* raw = nullptr; size_t size = 0;
        const int result = nng_recv(g_tcp.socket, &raw, &size, NNG_FLAG_NONBLOCK | NNG_FLAG_ALLOC);
        if (result == NNG_EAGAIN) break;
        if (result) { g_tcp.status = std::string("NNG 接收失败: ") + nng_strerror(result); stop_tcp(); return; }
        ++g_tcp.messages; g_tcp.last_size = size;
        const auto* bytes = static_cast<const uint8_t*>(raw);
        g_tcp.last_preview = packet_preview(bytes, size);
        bool fill = size && std::all_of(bytes, bytes + size, [](uint8_t b) { return b == 0x33; });
        if (fill) { ++g_tcp.idle_messages; g_tcp.status = "收到 0x33 空闲消息"; nng_free(raw, size); continue; }
        ++g_tcp.valid_messages;
        const size_t message_asm = count_asm(bytes, size);
        g_tcp.sync_words += message_asm;
        size_t cursor = 0, frames = 0;
        while (cursor + ggak::FRAME <= size) {
            size_t sync = cursor;
            while (sync + CADU_ASM.size() <= size && !std::equal(CADU_ASM.begin(), CADU_ASM.end(), bytes + sync)) ++sync;
            if (sync + ggak::FRAME > size) break;
            if (sync > cursor) g_tcp.unsynced_bytes += sync - cursor;
            std::array<uint8_t, ggak::FRAME> frame{};
            std::copy_n(bytes + sync, ggak::FRAME, frame.begin());
            if (frame[4] == 0x77 || frame[4] == 0x88) ++g_tcp.idle_frames;
            if (g_tcp.decoder.previous_coarse >= 0 && g_tcp.decoder.previous_coarse - int(ggak::be16(frame.data() + 10)) > 50000)
                g_tcp.decoder.coarse_offset += 65536;
            const uint64_t pass = g_data.pass, fail = g_data.fail;
            ggak::decode_frame(frame, g_strict, g_data, g_tcp.decoder);
            if (g_data.pass > pass) ++g_tcp.checksum_pass;
            if (g_data.fail > fail) ++g_tcp.checksum_fail;
            ++g_tcp.decode_frames; ++frames; cursor = sync + ggak::FRAME;
        }
        if (!frames) ++g_tcp.bad_messages;
        g_tcp.status = frames ? "收到 NNG 消息，提取 " + std::to_string(frames) + " 帧" :
                                (message_asm ? "找到 ASM，但没有完整 CADU" : "未找到 CADU ASM");
        nng_free(raw, size);
    }
}

void log_error(const char* message) {
    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "%s (EGL error 0x%04x)", message, eglGetError());
}

bool init_display(android_app* app) {
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Initializing EGL (SDK %d)",
                        android_get_device_api_level());
    const EGLint attributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_NONE };
    EGLConfig config = nullptr;
    EGLint count = 0;
    g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_display == EGL_NO_DISPLAY) { log_error("eglGetDisplay failed"); return false; }
    if (eglInitialize(g_display, nullptr, nullptr) != EGL_TRUE) { log_error("eglInitialize failed"); return false; }
    if (eglChooseConfig(g_display, attributes, &config, 1, &count) != EGL_TRUE || count == 0) {
        log_error("No OpenGL ES 3 EGL configuration"); return false;
    }
    EGLint format = 0;
    if (eglGetConfigAttrib(g_display, config, EGL_NATIVE_VISUAL_ID, &format) != EGL_TRUE) {
        log_error("eglGetConfigAttrib failed"); return false;
    }
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, format);
    const EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    g_surface = eglCreateWindowSurface(g_display, config, app->window, nullptr);
    if (g_surface == EGL_NO_SURFACE) { log_error("eglCreateWindowSurface failed"); return false; }
    g_context = eglCreateContext(g_display, config, EGL_NO_CONTEXT, context_attributes);
    if (g_context == EGL_NO_CONTEXT) { log_error("eglCreateContext failed"); return false; }
    if (eglMakeCurrent(g_display, g_surface, g_surface, g_context) != EGL_TRUE) {
        log_error("eglMakeCurrent failed"); return false;
    }
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "EGL/OpenGL ES initialized");
    return true;
}

void shutdown_display() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
    }
    if (g_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_context != EGL_NO_CONTEXT) eglDestroyContext(g_display, g_context);
        if (g_surface != EGL_NO_SURFACE) eglDestroySurface(g_display, g_surface);
        eglTerminate(g_display);
    }
    g_display = EGL_NO_DISPLAY; g_surface = EGL_NO_SURFACE; g_context = EGL_NO_CONTEXT;
}

void configure_touch_ui(android_app* app) {
    const int density = AConfiguration_getDensity(app->config);
    const float density_scale = density > 0 && density != ACONFIGURATION_DENSITY_DEFAULT
        ? static_cast<float>(density) / 240.0f : 2.0f;
    g_ui_scale = std::clamp(density_scale, 1.75f, 2.5f);

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(g_ui_scale);
    style.WindowRounding = 0.0f;
    style.FrameRounding = 5.0f * g_ui_scale;
    style.TabRounding = 5.0f * g_ui_scale;
    style.GrabMinSize = 18.0f * g_ui_scale;
    style.TouchExtraPadding = ImVec2(5.0f * g_ui_scale, 5.0f * g_ui_scale);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Touch UI scale %.2f (density %d)",
                        g_ui_scale, density);
}

void load_font(android_app* app, ImGuiIO& io) {
    constexpr const char* font_asset = "fonts/NotoSansSC-UI.ttf";
    AAsset* asset = AAssetManager_open(app->activity->assetManager, font_asset, AASSET_MODE_BUFFER);
    if (asset) {
        const off_t size = AAsset_getLength(asset);
        void* data = size > 0 ? IM_ALLOC(static_cast<size_t>(size)) : nullptr;
        const int64_t read = data ? AAsset_read(asset, data, static_cast<size_t>(size)) : 0;
        AAsset_close(asset);
        if (read == size) {
            ImFontGlyphRangesBuilder ranges;
            ranges.AddRanges(io.Fonts->GetGlyphRangesDefault());
            ranges.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
            ranges.AddText("打开文件加载并解码解码器严格校验概览请选择尚未加载总帧通过失败"
                           "填充磁场点粒子计数包太阳总辐照度无数据完成读取无法中没有完整未知"
                           "状态配置数据图表数据点仪器实时输入离线版当前结果个块估计丢帧计数器跳号"
                           "TCP地址端口连接断开NNG消息Idle空闲无完整帧收到提取服务端启动等待已手动"
                           "失败保存网络调试校验有效数据默认数据类型连接中连接失败未连接"
                           "图表缩放拖动滚轮平滑不平滑轻度中度强度复位有效点太阳能量谱"
                           "总帧校验通过校验失败填充帧源编号数据点概览当前解码结果详情仪器"
                           "MessagesCADUFramesResetZoomDragWheel");
            ranges.BuildRanges(&g_font_ranges);
            if (io.Fonts->AddFontFromMemoryTTF(data, static_cast<int>(size), 20.0f * g_ui_scale, nullptr,
                                               g_font_ranges.Data)) {
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Loaded bundled CJK font (%lld bytes)",
                                    static_cast<long long>(size));
                return;
            }
        }
        if (data) IM_FREE(data);
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Could not load bundled CJK font");
    } else {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Missing asset: %s", font_asset);
    }
    io.Fonts->AddFontDefault();
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Loaded bundled ImGui default font");
}

void open_picker() {
    JNIEnv* env = nullptr;
    g_app->activity->vm->AttachCurrentThread(&env, nullptr);
    jobject activity = g_app->activity->clazz;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, "openCaduPicker", "()V");
    env->CallVoidMethod(activity, method);
    env->DeleteLocalRef(type);
    g_app->activity->vm->DetachCurrentThread();
}

void metric(const char* label, uint64_t value) {
    ImGui::TextDisabled("%s", label);
    ImGui::Text("%llu", static_cast<unsigned long long>(value));
}

void plot_multi(const char* title, const char* id,
                const std::vector<const std::vector<float>*>& series,
                const std::vector<ImVec4>& colors,
                const std::vector<const char*>& names) {
    size_t count = 0; for (const auto* values : series) if (values) count = std::max(count, values->size());
    if (!count) { ImGui::TextDisabled("无数据"); return; }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(std::max(240.0f, avail.x), std::max(260.0f, avail.y - 8.0f));
    ImGui::InvisibleButton(id, size);
    // The chart hit area covers the floating toolbar. Allow later toolbar
    // items to win hit testing when the user taps a control over the chart.
    ImGui::SetItemAllowOverlap();
    const bool chart_hovered = ImGui::IsItemHovered();
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float left = min.x + 52.0f, right = max.x - 10.0f, top = min.y + 30.0f;
    const float bottom = max.y - 48.0f, width = std::max(1.0f, right - left), height = std::max(1.0f, bottom - top);
    struct View { float zx = 1.0f, zy = 1.0f, cx = .5f, cy = .5f; };
    static std::unordered_map<std::string, View> views;
    View& view = views[id];
    static std::unordered_map<std::string, int> smooth_windows;
    int& smooth_window = smooth_windows[id];
    const float hx = .5f / view.zx, hy = .5f / view.zy;
    view.cx = std::clamp(view.cx, hx, 1.0f - hx); view.cy = std::clamp(view.cy, hy, 1.0f - hy);
    const float xs = view.cx - hx, xe = view.cx + hx, ys = view.cy - hy, ye = view.cy + hy;
    float lo = FLT_MAX, hi = -FLT_MAX;
    for (const auto* values : series) if (values) for (float value : *values) { lo = std::min(lo, value); hi = std::max(hi, value); }
    if (hi <= lo) hi = lo + 1.0f;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(15, 15, 16, 255)); draw->AddRect(min, max, IM_COL32(70, 74, 80, 255));
    draw->AddText(ImVec2((min.x + max.x) * .5f - ImGui::CalcTextSize(title).x * .5f, min.y + 7), IM_COL32(240,240,240,255), title);
    // Keep chart controls over the plot without consuming chart layout space.
    const ImVec2 toolbar_min(std::max(min.x + 8.0f, max.x - 410.0f * g_ui_scale), min.y + 8.0f);
    ImGui::SetCursorScreenPos(toolbar_min);
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(20, 22, 25, 220));
    ImGui::BeginGroup();
    bool toolbar_activated = false;
    if (ImGui::Button("X+", ImVec2(54.0f * g_ui_scale, 44.0f * g_ui_scale))) {
        view.zx = std::min(256.0f, view.zx * 1.5f);
        toolbar_activated = true;
    }
    ImGui::SameLine(0, 5.0f * g_ui_scale);
    if (ImGui::Button("Y+", ImVec2(54.0f * g_ui_scale, 44.0f * g_ui_scale))) {
        view.zy = std::min(256.0f, view.zy * 1.5f);
        toolbar_activated = true;
    }
    ImGui::SameLine(0, 5.0f * g_ui_scale);
    if (ImGui::Button("复位", ImVec2(78.0f * g_ui_scale, 44.0f * g_ui_scale))) {
        view = View{};
        toolbar_activated = true;
    }
    ImGui::SameLine(0, 5.0f * g_ui_scale);
    const char* smooth_names[] = {"不平滑", "轻度平滑", "中度平滑", "强度平滑"};
    ImGui::SetNextItemWidth(118.0f * g_ui_scale);
    if (ImGui::BeginCombo("##smooth", smooth_names[smooth_window])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(smooth_names[i], smooth_window == i)) {
                smooth_window = i;
                toolbar_activated = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndGroup();
    ImGui::PopStyleColor();
    ImGui::PopID();
    const ImVec2 toolbar_max(toolbar_min.x + 319.0f * g_ui_scale,
                             toolbar_min.y + 44.0f * g_ui_scale);
    const bool toolbar_hovered = ImGui::IsMouseHoveringRect(toolbar_min, toolbar_max, true);
    // The chart's invisible hit area is deliberately behind the toolbar. A
    // touch that starts on a control must never become a chart drag or reset.
    if (chart_hovered && !toolbar_hovered && !toolbar_activated) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f) {
            const float factor = io.MouseWheel > 0 ? 1.25f : .8f;
            const float mx = std::clamp((io.MousePos.x - left) / width, 0.0f, 1.0f);
            const float my = std::clamp((bottom - io.MousePos.y) / height, 0.0f, 1.0f);
            const float ox = view.zx, oy = view.zy;
            view.zx = std::clamp(view.zx * factor, 1.0f, 256.0f);
            view.zy = std::clamp(view.zy * factor, 1.0f, 256.0f);
            view.cx += (mx - .5f) * (1.0f / ox - 1.0f / view.zx);
            view.cy += (my - .5f) * (1.0f / oy - 1.0f / view.zy);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            view.cx -= io.MouseDelta.x / width / view.zx;
            view.cy += io.MouseDelta.y / height / view.zy;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) view = View{};
    }
    ImGui::SetCursorScreenPos({min.x, max.y});
    for (int i = 0; i <= 10; ++i) {
        const float x = left + width * i / 10.0f; draw->AddLine({x, top}, {x, bottom}, IM_COL32(55,57,60,180));
        char text[32]; std::snprintf(text, sizeof(text), "%zu", static_cast<size_t>(xs * count + (xe - xs) * count * i / 10.0f));
        draw->AddText({x - 10, bottom + 6}, IM_COL32(220,220,220,255), text);
    }
    for (int i = 0; i <= 6; ++i) {
        const float y = bottom - height * i / 6.0f; draw->AddLine({left,y},{right,y},IM_COL32(55,57,60,180));
        char text[32]; std::snprintf(text, sizeof(text), "%.3g", lo + (hi-lo)*(ys+(ye-ys)*i/6.0f));
        draw->AddText({min.x + 5, y - 8}, IM_COL32(220,220,220,255), text);
    }
    draw->PushClipRect({left,top},{right,bottom},true);
    const size_t columns = std::max<size_t>(2, static_cast<size_t>(width));
    for (size_t s = 0; s < series.size(); ++s) {
        if (!series[s]) continue;
        const auto& values = *series[s]; if (values.empty()) continue;
        const size_t smoothing = smooth_window == 0 ? 1 : (smooth_window == 1 ? 3 : (smooth_window == 2 ? 5 : 9));
        const size_t first = std::min(values.size()-1, static_cast<size_t>(xs * (values.size()-1)));
        const size_t last = std::min(values.size()-1, static_cast<size_t>(xe * (values.size()-1)));
        const size_t point_count = last - first + 1;
        const size_t step = std::max<size_t>(1, point_count / columns);
        ImVec2 previous{}; bool have = false;
        const ImU32 line_color = ImGui::ColorConvertFloat4ToU32(colors[s]);
        for (size_t column = 0; column < columns; ++column) {
            const size_t begin = std::min(last, first + column * point_count / columns);
            const size_t end = std::min(last + 1, std::max(begin + 1, first + (column + 1) * point_count / columns));
            auto sample = [&](size_t index) {
                if (smoothing == 1) return values[index];
                const size_t half = smoothing / 2;
                const size_t from = index > half ? index - half : 0;
                const size_t to = std::min(values.size() - 1, index + half);
                double sum = 0.0;
                for (size_t j = from; j <= to; ++j) sum += values[j];
                return static_cast<float>(sum / static_cast<double>(to - from + 1));
            };
            float column_lo = sample(begin), column_hi = column_lo;
            for (size_t i = begin + 1; i < end; ++i) {
                const float value = sample(i);
                column_lo = std::min(column_lo, value);
                column_hi = std::max(column_hi, value);
            }
            const float ratio = point_count <= 1 ? 0.5f :
                static_cast<float>(begin - first) / static_cast<float>(std::max<size_t>(1, point_count - 1));
            const float x = left + width * ratio;
            const float y_lo = bottom - height * ((column_lo - lo) / (hi - lo) - ys) / std::max(.0001f, ye - ys);
            const float y_hi = bottom - height * ((column_hi - lo) / (hi - lo) - ys) / std::max(.0001f, ye - ys);
            draw->AddLine({x, y_lo}, {x, y_hi}, line_color, 1.8f);
            if (!have) { previous = {x, (y_lo + y_hi) * .5f}; have = true; }
            else { draw->AddLine(previous, {x, (y_lo + y_hi) * .5f}, line_color, 1.2f); previous = {x, (y_lo + y_hi) * .5f}; }
        }
    }
    draw->PopClipRect();
    float legend_x = left;
    for (size_t i=0; i<series.size(); ++i) { draw->AddRectFilled({legend_x,bottom+25},{legend_x+14,bottom+28},ImGui::ColorConvertFloat4ToU32(colors[i])); draw->AddText({legend_x+19,bottom+20},IM_COL32(220,225,235,230),names[i]); legend_x += 28 + ImGui::CalcTextSize(names[i]).x; }
    ImGui::TextDisabled("有效点: %zu   X x%.1f  Y x%.1f   平滑: %s", count, view.zx, view.zy, smooth_names[smooth_window]);
}

void plot(const char* title, const std::vector<float>& values) {
    plot_multi(title, "##plot", {&values}, {ImVec4(.25f,.8f,1,1)}, {"data"});
}

void draw_ui() {
    poll_tcp();
    std::string path, name;
    { std::lock_guard<std::mutex> lock(g_file_mutex); path.swap(g_pending_path); name.swap(g_pending_name); }
    if (!name.empty()) {
        if (path.empty()) g_status = name;
        else { g_data = {}; g_status = ggak::decode_file(path, g_strict, g_data, g_status) ? "解码完成: " + name : g_status; }
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("GGAK Android", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("GGAK CADU 解码器"); ImGui::SameLine(); ImGui::TextDisabled("%s", g_status.c_str());
    ImGui::Separator();
    ImGui::BeginChild("sidebar", ImVec2(std::min(360.0f, io.DisplaySize.x * 0.30f), 0), true,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextUnformatted("文件"); ImGui::Separator();
    ImGui::TextWrapped("%s", g_status.c_str());
    if (ImGui::Button("打开文件...##open_file", ImVec2(-1, 48.0f * g_ui_scale))) open_picker();
    ImGui::Checkbox("严格校验##strict", &g_strict);
    ImGui::Spacing(); ImGui::TextUnformatted("网络连接"); ImGui::Separator();
    static char host[64] = "127.0.0.1";
    static int port = 8888;
    ImGui::InputText("地址##tcp_host", host, sizeof(host));
    ImGui::InputInt("端口##tcp_port", &port);
    if (!g_tcp.connected) {
        if (ImGui::Button("连接 NNG##tcp_connect", ImVec2(-1, 52.0f * g_ui_scale))) {
            g_tcp.host = host; g_tcp.port = std::clamp(port, 1, 65535); start_tcp();
        }
    } else if (ImGui::Button("断开 NNG##tcp_disconnect", ImVec2(-1, 52.0f * g_ui_scale))) stop_tcp();
    ImGui::TextWrapped("%s", g_tcp.status.c_str());
    ImGui::Text("消息: %llu  CADU: %llu", (unsigned long long)g_tcp.messages, (unsigned long long)g_tcp.decode_frames);
    if (ImGui::CollapsingHeader("网络调试详情")) {
        ImGui::Text("有效消息: %llu  异常消息: %llu", (unsigned long long)g_tcp.valid_messages,
                    (unsigned long long)g_tcp.bad_messages);
        ImGui::Text("空闲消息: %llu  空闲帧: %llu", (unsigned long long)g_tcp.idle_messages,
                    (unsigned long long)g_tcp.idle_frames);
        ImGui::Text("ASM: %llu  未同步字节: %llu", (unsigned long long)g_tcp.sync_words,
                    (unsigned long long)g_tcp.unsynced_bytes);
        ImGui::Text("校验通过/失败: %llu / %llu", (unsigned long long)g_tcp.checksum_pass,
                    (unsigned long long)g_tcp.checksum_fail);
        ImGui::Text("NNG pipe: %s  建立/断开: %llu / %llu", g_tcp.pipe_connected ? "已连接" : "未连接",
                    (unsigned long long)g_tcp.pipe_adds, (unsigned long long)g_tcp.pipe_removes);
        ImGui::TextWrapped("最近消息: %s", g_tcp.last_preview.c_str());
    }
    ImGui::Spacing(); ImGui::TextUnformatted("状态"); ImGui::Separator();
    if (g_data.total) {
        ImGui::TextWrapped("配置: %s", g_data.profile.c_str());
        ImGui::Text("源编号: 0x%02X", g_data.source);
        ImGui::Text("总帧: %llu", static_cast<unsigned long long>(g_data.total));
    } else {
        ImGui::TextDisabled("尚未加载 CADU 文件");
    }
    ImGui::Spacing(); ImGui::TextUnformatted("主要数据"); ImGui::Separator();
    ImGui::TextDisabled("详细数据和仪器图表请在右侧标签页查看");
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("workspace", ImVec2(0, 0), false);
    if (ImGui::BeginTabBar("main_tabs")) {
        if (ImGui::BeginTabItem("概览##overview_tab")) {
            if (!g_data.total) ImGui::TextDisabled("尚未加载 CADU 文件");
            else {
                ImGui::Text("配置: %s    源编号: 0x%02X", g_data.profile.c_str(), g_data.source);
                ImGui::Spacing();
                if (ImGui::BeginTable("metrics", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                    ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableNextColumn(); metric("总帧", g_data.total);
                    ImGui::TableNextColumn(); metric("校验通过", g_data.pass);
                    ImGui::TableNextColumn(); metric("校验失败", g_data.fail);
                    ImGui::TableNextColumn(); metric("填充帧", g_data.fill);
                    ImGui::EndTable();
                }
                ImGui::Spacing(); ImGui::TextUnformatted("数据点概览");
                ImGui::BulletText("FM-VE 磁场: %d", static_cast<int>(g_data.mag[0].size()));
                ImGui::BulletText("GALS-VE 粒子: %d", static_cast<int>(g_data.particle[0].size()));
                ImGui::BulletText("SKIF-VE/V ESA: %d 包", static_cast<int>(g_data.esa_v.size()));
                ImGui::BulletText("SKIF-VE/G ESA: %d 包", static_cast<int>(g_data.esa_g.size()));
                ImGui::BulletText("ISP-2M TSI: %d", static_cast<int>(g_data.tsi.size()));
                ImGui::BulletText("SKIF-VE SER: %d", static_cast<int>(g_data.ser.size()));
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("图表##charts_tab")) {
            if (!g_data.total) ImGui::TextDisabled("尚未加载 CADU 文件");
            else if (ImGui::BeginTabBar("instrument_tabs")) {
                if (ImGui::BeginTabItem("FM-VE 磁场")) { plot_multi("FM-VE 磁场", "##plot_mag", {&g_data.mag[0],&g_data.mag[1],&g_data.mag[2],&g_data.mag[3],&g_data.mag_voltage}, {ImVec4(1,.4f,.3f,1),ImVec4(.3f,1,.4f,1),ImVec4(.3f,.6f,1,1),ImVec4(1,.8f,.2f,1),ImVec4(.8f,.5f,1,1)}, {"X","Y","Z","B","V"}); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("GALS-VE 粒子")) { plot_multi("GALS-VE 粒子计数", "##plot_particle", {&g_data.particle[0],&g_data.particle[1],&g_data.particle[2],&g_data.particle[3],&g_data.particle[4],&g_data.particle[5],&g_data.particle[6],&g_data.particle[7]}, {ImVec4(1,.4f,.3f,1),ImVec4(.3f,1,.4f,1),ImVec4(.3f,.6f,1,1),ImVec4(1,.8f,.2f,1),ImVec4(.8f,.5f,1,1),ImVec4(.4f,1,.9f,1),ImVec4(1,.4f,.8f,1),ImVec4(.7f,.8f,1,1)}, {"CH1","CH2","CH3","CH4","CH5","CH6","CH7","CH8"}); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("ISP-2M")) { plot("太阳总辐照度", g_data.tsi); ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("SER")) { plot("SKIF-VE SER", g_data.ser); ImGui::EndTabItem(); }
                ImGui::EndTabBar();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("数据##data_tab")) {
            ImGui::TextUnformatted("当前解码结果"); ImGui::Separator();
            ImGui::Text("FM-VE: %d 个点", static_cast<int>(g_data.mag[0].size()));
            ImGui::Text("GALS-VE: %d 个点", static_cast<int>(g_data.particle[0].size()));
            ImGui::Text("SKIF-VE/V ESA: %d 个包", static_cast<int>(g_data.esa_v.size()));
            ImGui::Text("SKIF-VE/G ESA: %d 个包", static_cast<int>(g_data.esa_g.size()));
            ImGui::Text("ISP-2M: %d 个点", static_cast<int>(g_data.tsi.size()));
            ImGui::Text("SER: %d 个点", static_cast<int>(g_data.ser.size()));
            ImGui::Text("计数器跳号: %llu 次，估计丢帧: %llu",
                        static_cast<unsigned long long>(g_data.frame_gaps),
                        static_cast<unsigned long long>(g_data.missing_frames));
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::End();
}

void handle_command(android_app*, int32_t command) {
    __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, "App command: %d", command);
    if (command == APP_CMD_TERM_WINDOW && g_display != EGL_NO_DISPLAY) shutdown_display();
}

int32_t handle_input(android_app*, AInputEvent* event) {
    return ImGui_ImplAndroid_HandleInputEvent(event);
}
} // namespace

extern "C" JNIEXPORT void JNICALL
Java_io_github_ggak_decoder_MainActivity_nativeSetFile(JNIEnv* env, jclass, jstring path, jstring name) {
    const char* path_text = env->GetStringUTFChars(path, nullptr);
    const char* name_text = env->GetStringUTFChars(name, nullptr);
    { std::lock_guard<std::mutex> lock(g_file_mutex); g_pending_path = path_text; g_pending_name = name_text; }
    env->ReleaseStringUTFChars(path, path_text);
    env->ReleaseStringUTFChars(name, name_text);
}

void android_main(android_app* app) {
    app_dummy();
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "android_main started; activity=%p window=%p",
                        app->activity, app->window);
    g_app = app;
    app->onAppCmd = handle_command;
    app->onInputEvent = handle_input;
    while (!app->destroyRequested) {
        int events; android_poll_source* source;
        while (ALooper_pollOnce(g_display == EGL_NO_DISPLAY ? -1 : 0, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source) source->process(app, source);
            if (app->destroyRequested) break;
            if (g_display == EGL_NO_DISPLAY && app->window) {
                if (!init_display(app)) {
                    shutdown_display();
                    ANativeActivity_finish(app->activity);
                    return;
                }
                IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGui::StyleColorsDark();
                ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
                configure_touch_ui(app);
                load_font(app, io);
                GLint max_texture_size = 0;
                glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "GL renderer=%s; max texture=%d",
                                    glGetString(GL_RENDERER), max_texture_size);
                if (!ImGui_ImplAndroid_Init(app->window) || !ImGui_ImplOpenGL3_Init("#version 300 es")) {
                    __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "ImGui backend initialization failed");
                    shutdown_display();
                    ANativeActivity_finish(app->activity);
                    return;
                }
                break;
            }
        }
        if (app->destroyRequested) break;
        if (g_display == EGL_NO_DISPLAY) continue;
        ImGui_ImplOpenGL3_NewFrame(); ImGui_ImplAndroid_NewFrame(); ImGui::NewFrame(); draw_ui(); ImGui::Render();
        int width, height; eglQuerySurface(g_display, g_surface, EGL_WIDTH, &width); eglQuerySurface(g_display, g_surface, EGL_HEIGHT, &height);
        glViewport(0, 0, width, height); glClearColor(.055f, .065f, .08f, 1); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); eglSwapBuffers(g_display, g_surface);
    }
    if (g_display != EGL_NO_DISPLAY) shutdown_display();
}
