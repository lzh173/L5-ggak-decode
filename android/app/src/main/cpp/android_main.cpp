#include "ggak_core.h"
#include "imgui.h"
#include "backends/imgui_impl_android.h"
#include "backends/imgui_impl_opengl3.h"

#include <android/log.h>
#include <android/api-level.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <algorithm>
#include <cfloat>
#include <mutex>
#include <string>
#include <unistd.h>

namespace {
constexpr const char* LOG_TAG = "GGAKDecoder";
android_app* g_app = nullptr;
EGLDisplay g_display = EGL_NO_DISPLAY;
EGLSurface g_surface = EGL_NO_SURFACE;
EGLContext g_context = EGL_NO_CONTEXT;
std::mutex g_file_mutex;
std::string g_pending_path, g_pending_name;
ggak::Data g_data;
std::string g_status = "请选择 CADU 文件";
bool g_strict = false;

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

void load_font(ImGuiIO& io) {
    // Vendor system fonts are not an Android API. Some Android 16 ROMs expose
    // a readable NotoSansCJK TTC that stb_truetype cannot parse, which aborts
    // later when ImGui creates the font texture. Keep startup independent of
    // those implementation-specific files.
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

void plot(const char* title, const std::vector<float>& values) {
    ImGui::TextUnformatted(title);
    if (values.empty()) { ImGui::TextDisabled("无数据"); return; }
    const ImVec2 available = ImGui::GetContentRegionAvail();
    ImGui::PlotLines("##plot", values.data(), int(values.size()), 0, nullptr, 0.0f, FLT_MAX,
                     ImVec2(std::max(240.0f, available.x), std::max(220.0f, available.y - 8.0f)));
}

void draw_ui() {
    std::string path, name;
    { std::lock_guard<std::mutex> lock(g_file_mutex); path.swap(g_pending_path); name.swap(g_pending_name); }
    if (!name.empty()) {
        if (path.empty()) g_status = name;
        else { g_data = {}; g_status = ggak::decode_file(path, g_strict, g_data, g_status) ? "解码完成: " + name : g_status; }
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("GGAK Android", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize);
    ImGui::TextUnformatted("GGAK CADU 解码器"); ImGui::SameLine(); ImGui::TextDisabled("%s", g_status.c_str());
    ImGui::Separator();
    if (ImGui::Button("打开 CADU...", {180, 44})) open_picker();
    ImGui::SameLine(); ImGui::Checkbox("严格校验", &g_strict);
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("概览")) {
            if (!g_data.total) ImGui::TextDisabled("尚未加载 CADU 文件");
            else if (ImGui::BeginTable("metrics", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                ImGui::TableNextColumn(); metric("总帧", g_data.total);
                ImGui::TableNextColumn(); metric("校验通过", g_data.pass);
                ImGui::TableNextColumn(); metric("校验失败", g_data.fail);
                ImGui::TableNextColumn(); metric("填充帧", g_data.fill);
                ImGui::TableNextColumn(); metric("FM-VE 点", g_data.mag[0].size());
                ImGui::TableNextColumn(); metric("GALS-VE 点", g_data.particle[0].size());
                ImGui::TableNextColumn(); metric("ESA-V 包", g_data.esa_v.size());
                ImGui::TableNextColumn(); metric("ESA-G 包", g_data.esa_g.size());
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("FM-VE")) { plot("磁场 |B|", g_data.mag[0]); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("GALS-VE")) { plot("粒子计数", g_data.particle[0]); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("ISP-2M")) { plot("太阳总辐照度", g_data.tsi); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("SER")) { plot("SKIF-VE SER", g_data.ser); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
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
                load_font(io);
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
