// Sanctum — a minimal hot-reload shader viewer. One window, one fullscreen
// pass per scene, uniform push per frame. Slang compiles via goose-build
// (sibling checkout); the C++ side is a thin SDL3 GPU renderer.
//
// Design constraints:
//   - No ImGui, no UI toolkit. Keyboard only (LEFT/RIGHT switch scenes, ESC quit).
//   - Hot reload: poll shader mtimes; on change rerun `make shaders`, rebuild
//     pipelines between frames (device idle), keep the last good plan on
//     compile errors (diagnostics go to stdout).
//   - The goose reflection (via `--analyze` StageModel JSON) is the authority
//     for uniform block size and resource counts; the hand-mirrored
//     SceneUniforms in C++ is asserted against it at load. Drift fails loud.
//
// Scene convention (shaders/scenes/<name>.slang): one file, two entry points
// (vertMain, pixelMain), `import "common"` for SceneUniforms at binding 0.
// The fragment stage must consume exactly one uniform block; the vertex stage
// may consume it or not (Metal drops unused bindings — we push accordingly).
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "goose/adapters/sdl_gpu.hpp"
#include "goose/model.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr Uint64 kReloadPollMs = 250;  // shader mtime poll cadence

[[noreturn]] void fail(const std::string& step) {
    std::fprintf(stderr, "sanctum: %s failed: %s\n", step.c_str(), SDL_GetError());
    std::exit(1);
}

// Hand mirror of shaders/common.slang SceneUniforms. Size asserted against
// the goose StageModel at scene load; keep both in sync. The fragment-stage
// block is 48 bytes because Slang pads float3 to 16-byte alignment.
struct Uniforms {
    float iResolution[4];   // float3 padded to 16 bytes by the shader layout
    float iTime;
    float iMouse[2];
    float iClick;
    float iAspect;
    float _pad[3];          // tail padding to the shader's 48-byte block
};
static_assert(sizeof(Uniforms) == 48, "SceneUniforms layout drifted from shaders/common.slang");

std::string read_file(const fs::path& path) {
    SDL_IOStream* stream = SDL_IOFromFile(path.string().c_str(), "rb");
    if (!stream) throw std::runtime_error("cannot open " + path.string());
    const std::size_t size = static_cast<std::size_t>(SDL_GetIOSize(stream));
    std::string data(size, '\0');
    if (size > 0 && SDL_ReadIO(stream, data.data(), size) != size) {
        SDL_CloseIO(stream);
        throw std::runtime_error("short read on " + path.string());
    }
    SDL_CloseIO(stream);
    return data;
}

#ifdef __APPLE__
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_MSL;
constexpr const char* kVertExt = ".vert.msl";
constexpr const char* kFragExt = ".frag.msl";
#else
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
constexpr const char* kVertExt = ".vert.spv";
constexpr const char* kFragExt = ".frag.spv";
#endif

SDL_GPUShader* create_shader(SDL_GPUDevice* device, const std::string& code,
                             SDL_GPUShaderStage stage, const char* entry,
                             Uint32 num_uniform_buffers) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(code.data());
    info.code_size = code.size();
    info.entrypoint = entry;
    info.format = kShaderFormat;
    info.stage = stage;
    info.num_samplers = 0;
    info.num_storage_textures = 0;
    info.num_storage_buffers = 0;
    info.num_uniform_buffers = num_uniform_buffers;
    return SDL_CreateGPUShader(device, &info);
}

// One scene: reflection models + shaders + pipeline. A scene that fails to
// load (compile error, drift) stays unloaded; the previous good plan holds.
struct Scene {
    std::string name;
    goose::StageModel vert_model;
    goose::StageModel frag_model;
    SDL_GPUShader* vert = nullptr;
    SDL_GPUShader* frag = nullptr;
    SDL_GPUGraphicsPipeline* pipeline = nullptr;
    Uint32 uniform_size = 0;   // rounded up to 256 (push-uniform alignment)
    bool vert_takes_uniforms = false;

    bool load(SDL_GPUDevice* device, SDL_GPUTextureFormat swapchain_format,
              const fs::path& generated_dir, const std::string& scene_name,
              std::string& error);
    void destroy(SDL_GPUDevice* device) {
        if (pipeline) SDL_ReleaseGPUGraphicsPipeline(device, pipeline);
        if (vert) SDL_ReleaseGPUShader(device, vert);
        if (frag) SDL_ReleaseGPUShader(device, frag);
        pipeline = nullptr;
        vert = nullptr;
        frag = nullptr;
    }
};

bool Scene::load(SDL_GPUDevice* device, SDL_GPUTextureFormat swapchain_format,
                 const fs::path& generated_dir, const std::string& scene_name,
                 std::string& error) {
    auto read_model = [&](const char* stage) -> std::optional<goose::StageModel> {
        try {
            return goose::load_stage_model_file(generated_dir /
                                                (scene_name + stage + ".model.json"));
        } catch (const std::exception& e) {
            error = e.what();
            return std::nullopt;
        }
    };
    std::optional<goose::StageModel> vert_m = read_model(".vert");
    std::optional<goose::StageModel> frag_m = read_model(".frag");
    if (!vert_m || !frag_m) {
        error = "missing stage models for '" + scene_name + "': " + error;
        return false;
    }
    vert_model = std::move(*vert_m);
    frag_model = std::move(*frag_m);

    // The reflection is the authority; enforce the scene convention now.
    if (frag_model.uniform_blocks.size() != 1 ||
        vert_model.uniform_blocks.size() > 1) {
        error = "'" + scene_name + "': expected exactly one uniform block in the " +
                "fragment stage (vertex stage optional)";
        return false;
    }
    if (!vert_model.resources.empty() || !frag_model.resources.empty()) {
        error = "'" + scene_name + "': textures/samplers not supported yet";
        return false;
    }
    const std::int64_t fsize = frag_model.uniform_blocks[0].size;
    if (fsize != static_cast<std::int64_t>(sizeof(Uniforms))) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "'%s': uniform size mismatch (shader %lld bytes, C++ %zu)",
                      scene_name.c_str(), static_cast<long long>(fsize), sizeof(Uniforms));
        error = buf;
        return false;
    }
    vert_takes_uniforms = vert_model.uniform_blocks.size() == 1;
    if (vert_takes_uniforms && vert_model.uniform_blocks[0].size != fsize) {
        error = "'" + scene_name + "': vertex and fragment uniform blocks differ";
        return false;
    }
    uniform_size = static_cast<Uint32>((fsize + 255) & ~255);

    try {
        const std::string vcode = read_file(generated_dir / (scene_name + kVertExt));
        const std::string fcode = read_file(generated_dir / (scene_name + kFragExt));
        vert = create_shader(device, vcode, SDL_GPU_SHADERSTAGE_VERTEX,
                             vert_model.entry_point.c_str(),
                             vert_takes_uniforms ? 1u : 0u);
        frag = create_shader(device, fcode, SDL_GPU_SHADERSTAGE_FRAGMENT,
                             frag_model.entry_point.c_str(), 1u);
    } catch (const std::exception& e) {
        error = e.what();
        destroy(device);
        return false;
    }
    if (!vert || !frag) {
        error = "SDL_CreateGPUShader failed";
        destroy(device);
        return false;
    }
    SDL_GPUGraphicsPipelineCreateInfo info{};
    SDL_GPUColorTargetDescription target{};
    target.format = swapchain_format;
    info.vertex_shader = vert;
    info.fragment_shader = frag;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.target_info.num_color_targets = 1;
    info.target_info.color_target_descriptions = &target;
    pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipeline) {
        error = "pipeline creation failed";
        destroy(device);
        return false;
    }
    return true;
}

std::vector<std::string> list_scenes() {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator("shaders/scenes")) {
        if (entry.is_regular_file() && entry.path().extension() == ".slang") {
            names.push_back(entry.path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

// Reruns `make shaders` (goose-build). Returns empty on success, else the
// tail of the build output — slangc diagnostics are the useful part.
std::string run_shader_build() {
    std::string out;
    FILE* pipe = ::popen("make shaders 2>&1", "r");
    if (!pipe) return "cannot spawn make shaders";
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
    const int rc = ::pclose(pipe);
    if (rc == 0) return {};
    if (out.size() > 4000) out.erase(0, out.size() - 4000);
    return "make shaders failed:\n" + out;
}

fs::file_time_type latest_mtime(const std::vector<std::string>& scenes) {
    fs::file_time_type latest = fs::file_time_type::min();
    for (const std::string& name : scenes) {
        std::error_code ec;
        const fs::file_time_type t =
            fs::last_write_time("shaders/scenes/" + name + ".slang", ec);
        if (!ec && t > latest) latest = t;
    }
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time("shaders/common.slang", ec);
    if (!ec && t > latest) latest = t;
    return latest;
}

}  // namespace

int main(int argc, char** argv) {
    bool benchmark = false;
    int benchmark_frames = 300;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--benchmark") == 0 && i + 1 < argc) {
            benchmark = true;
            benchmark_frames = std::atoi(argv[++i]);
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) fail("SDL_Init");
    SDL_GPUDevice* device =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_SPIRV, true, nullptr);
    if (!device) fail("SDL_CreateGPUDevice");
    SDL_Window* window = SDL_CreateWindow("Sanctum", kWindowWidth, kWindowHeight,
                                          SDL_WINDOW_RESIZABLE);
    if (!window) fail("SDL_CreateWindow");
    if (!SDL_ClaimWindowForGPUDevice(device, window)) fail("SDL_ClaimWindowForGPUDevice");
    const SDL_GPUTextureFormat swapchain_format =
        SDL_GetGPUSwapchainTextureFormat(device, window);

    const std::vector<std::string> scene_names = list_scenes();
    if (scene_names.empty()) {
        std::fprintf(stderr, "sanctum: no scenes in shaders/scenes/\n");
        return 1;
    }

    const fs::path generated_dir = "build/generated";
    std::vector<Scene> scenes(scene_names.size());
    std::vector<bool> loaded(scene_names.size(), false);
    size_t current = 0;

    auto try_load = [&](size_t index) {
        std::string error;
        scenes[index].name = scene_names[index];
        if (scenes[index].load(device, swapchain_format, generated_dir, scene_names[index], error)) {
            loaded[index] = true;
            std::printf("sanctum: scene '%s' ready\n", scene_names[index].c_str());
        } else {
            std::printf("sanctum: scene '%s' NOT loaded: %s\n", scene_names[index].c_str(),
                        error.c_str());
        }
    };
    try_load(current);

    Uniforms u{};
    u.iMouse[0] = 0.0f;
    u.iMouse[1] = 0.0f;
    u.iClick = 0.0f;

    const auto start = std::chrono::steady_clock::now();
    Uint64 last_poll = 0;
    fs::file_time_type last_mtime = latest_mtime(scene_names);
    int frame = 0;
    bool running = true;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                } else if (event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) {
                    const size_t dir = event.key.key == SDLK_RIGHT ? 1 : scene_names.size() - 1;
                    current = (current + dir) % scene_names.size();
                    if (!loaded[current]) try_load(current);
                }
            }
        }
        if (!running) break;

        // Hot reload: poll mtimes at a fixed cadence; rebuild and swap
        // pipelines only at a frame boundary with the GPU idle, so released
        // resources cannot still be in flight.
        const Uint64 now = SDL_GetTicks();
        if (now - last_poll >= kReloadPollMs) {
            last_poll = now;
            const fs::file_time_type mtime = latest_mtime(scene_names);
            if (mtime != last_mtime) {
                last_mtime = mtime;
                const std::string diag = run_shader_build();
                if (!diag.empty()) {
                    std::printf("sanctum: reload refused, keeping last good plan\n%s", diag.c_str());
                } else {
                    SDL_WaitForGPUIdle(device);
                    for (size_t i = 0; i < scenes.size(); ++i) {
                        scenes[i].destroy(device);
                        loaded[i] = false;
                    }
                    for (size_t i = 0; i < scenes.size(); ++i) try_load(i);
                    std::printf("sanctum: reloaded\n");
                }
            }
        }

        Scene& scene = scenes[current];
        if (!loaded[current]) {
            SDL_Delay(16);
            continue;
        }

        // Per-frame uniforms. Mouse in -1..1 with y up; click is edge-held.
        const std::chrono::duration<float> elapsed = std::chrono::steady_clock::now() - start;
        float mx = 0.0f, my = 0.0f;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        int wi = 0, hi = 0;
        SDL_GetWindowSizeInPixels(window, &wi, &hi);
        u.iMouse[0] = (mx / static_cast<float>(wi > 0 ? wi : 1)) * 2.0f - 1.0f;
        u.iMouse[1] = 1.0f - (my / static_cast<float>(hi > 0 ? hi : 1)) * 2.0f;
        u.iClick = (buttons & SDL_BUTTON_LMASK) ? 1.0f : 0.0f;

        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
        if (!cmd) fail("SDL_AcquireGPUCommandBuffer");
        SDL_GPUTexture* swapchain = nullptr;
        Uint32 w = 0, h = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swapchain, &w, &h)) {
            SDL_SubmitGPUCommandBuffer(cmd);
            continue;
        }
        if (!swapchain) {  // frames in flight exhausted; not an error
            if (!SDL_SubmitGPUCommandBuffer(cmd)) fail("SDL_SubmitGPUCommandBuffer");
            continue;
        }
        u.iResolution[0] = static_cast<float>(w);
        u.iResolution[1] = static_cast<float>(h);
        u.iResolution[2] = 1.0f;
        u.iResolution[3] = 0.0f;  // layout padding, never read by the shader
        u.iAspect = static_cast<float>(w) / static_cast<float>(h > 0 ? h : 1);

        SDL_GPUColorTargetInfo color_target{};
        color_target.texture = swapchain;
        color_target.clear_color = {0.02f, 0.02f, 0.03f, 1.0f};
        color_target.load_op = SDL_GPU_LOADOP_CLEAR;
        color_target.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color_target, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, scene.pipeline);
        if (scene.vert_takes_uniforms) {
            SDL_PushGPUVertexUniformData(cmd, 0, &u, scene.uniform_size);
        }
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, scene.uniform_size);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        if (!SDL_SubmitGPUCommandBuffer(cmd)) fail("SDL_SubmitGPUCommandBuffer");

        ++frame;
        const std::chrono::duration<float> total = std::chrono::steady_clock::now() - start;
        if (benchmark && frame >= benchmark_frames) {
            SDL_WaitForGPUIdle(device);
            std::printf("benchmark: %d frames in %.3fs (%.2f fps)\n", frame,
                        static_cast<double>(total.count()),
                        static_cast<double>(frame) / static_cast<double>(total.count()));
            running = false;
        }
    }

    SDL_WaitForGPUIdle(device);
    for (Scene& scene : scenes) scene.destroy(device);
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyWindow(window);
    SDL_DestroyGPUDevice(device);
    SDL_Quit();
    return 0;
}
