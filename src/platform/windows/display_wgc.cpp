/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>

// platform includes
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <sddl.h>
#include <UserEnv.h>

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (monitor_factory == nullptr ||
        FAILED(status = monitor_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(winrt::TimeSpan{ 10000000 / (config.framerate * 2) });
      }
      else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name)) {
      return -1;
    }
    if (platf::is_running_as_system() || platf::window_capture()) {
      remote = make_wgc_remote(this, config);
      if (!remote) {
        return -1;
      }
    } else if (dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    capture_e capture_status;
    if (remote) {
      wgc_remote_set_cursor_visible(*remote, cursor_visible);
      capture_status = wgc_remote_next_frame(*remote, timeout, &src, frame_qpc);
    } else {
      dup.set_cursor_visible(cursor_visible);
      capture_status = dup.next_frame(timeout, &src, frame_qpc);
    }
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return remote ? wgc_remote_release_frame(*remote) : dup.release_frame();
  }
}  // namespace platf::dxgi

/*
 * WGC while TunaStream runs as a service.
 *
 * TunaStream runs as SYSTEM, and Windows.Graphics.Capture refuses to capture for SYSTEM. So the
 * service starts a copy of tunastream.exe as the signed-in user ("tunastream.exe --wgc-helper ...")
 * that does the capture and copies each frame into one of three textures in shared graphics
 * memory. Two named pipes connect them: the helper announces the textures and each new frame on
 * one, and TunaStream sends cursor on/off on the other (closing it tells the helper to quit).
 * A keyed mutex on each texture keeps the two from touching the same texture at once.
 *
 * WGC can't see the secure desktop (UAC prompts, the lock and sign-in screens), so while one is
 * showing TunaStream captures with DDX instead and comes back to WGC afterwards. If the helper
 * can't start or stops, TunaStream uses DDX too.
 */
namespace platf::dxgi::wgc_ipc {
  constexpr std::uint32_t MAGIC = 0x43475754;  // "TWGC"
  constexpr int SLOTS = 3;

  enum : std::uint32_t {
    MSG_READY = 1,  ///< capture started
    MSG_RING = 2,  ///< new shared textures: handles[] are valid in the helper's process
    MSG_FRAME = 3,  ///< slot holds a new frame taken at qpc
  };

  enum : int {
    EXIT_DISPLAY_CHANGED = 9,  ///< the display changed size: capture it again
    EXIT_WINDOW_CLOSED = 10,  ///< the window being captured closed
  };

  struct msg_t {
    std::uint32_t magic;
    std::uint32_t type;
    std::uint32_t slot;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t format;
    std::uint64_t qpc;
    std::uint64_t handles[SLOTS];
  };
}  // namespace platf::dxgi::wgc_ipc

namespace {
  using namespace platf::dxgi::wgc_ipc;

  /// Helper side: the frames go out on one pipe; the other carries cursor on/off.
  struct helper_t {
    HANDLE frame_pipe = INVALID_HANDLE_VALUE;
    HANDLE ctrl_pipe = INVALID_HANDLE_VALUE;
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    winrt::com_ptr<ID3D11Texture2D> ring[SLOTS];
    winrt::com_ptr<IDXGIKeyedMutex> mutexes[SLOTS];
    HANDLE handles[SLOTS] {};
    winrt::com_ptr<ID3D11RenderTargetView> targets[SLOTS];
    D3D11_TEXTURE2D_DESC ring_desc {};
    int next_slot = 0;
    std::mutex lock;
    winrt::Windows::Graphics::SizeInt32 size {};

    // One window instead of a whole display: frames go in the middle of display-sized textures,
    // and the capture follows the window's size
    bool window_mode = false;
    UINT ring_width = 0;
    UINT ring_height = 0;
    winrt::IDirect3DDevice uwp_device {nullptr};
    winrt::Windows::Graphics::DirectX::DirectXPixelFormat pixel_format {};

    // The last frame sent, sent again now and then when nothing changes: a window that sits
    // still (a paused video, a page) otherwise gave only its first frame, and anything starting
    // to watch after that saw black
    int last_slot = -1;
    std::uint64_t last_sent_ms = 0;

    // Sizing the window to fill the picture, keeping its shape: a small game window isn't a
    // postage stamp in the middle, and a big one isn't cut off
    winrt::com_ptr<ID3D11VertexShader> vs;
    winrt::com_ptr<ID3D11PixelShader> ps;
    winrt::com_ptr<ID3D11SamplerState> sampler;
    winrt::com_ptr<ID3D11Texture2D> picture;  ///< the window's picture, readable by the shader
    winrt::com_ptr<ID3D11ShaderResourceView> picture_view;
    D3D11_TEXTURE2D_DESC picture_desc {};
    bool scaler_tried = false;
    bool scaler_ok = false;

    bool make_scaler() {
      static constexpr char source[] = R"(
Texture2D picture : register(t0);
SamplerState smooth : register(s0);
struct v2p { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
v2p vs_main(uint id : SV_VertexID) {
  v2p o;
  o.uv = float2((id << 1) & 2, id & 2);
  o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
  return o;
}
float4 ps_main(v2p i) : SV_Target {
  return float4(picture.Sample(smooth, i.uv).rgb, 1);
}
)";
      winrt::com_ptr<ID3DBlob> vs_code;
      winrt::com_ptr<ID3DBlob> ps_code;
      winrt::com_ptr<ID3DBlob> errors;
      if (FAILED(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs_code.put(), errors.put())) ||
          FAILED(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps_code.put(), errors.put())) ||
          FAILED(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, vs.put())) ||
          FAILED(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, ps.put()))) {
        return false;
      }
      D3D11_SAMPLER_DESC sd {};
      sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
      sd.MaxLOD = D3D11_FLOAT32_MAX;
      return SUCCEEDED(device->CreateSamplerState(&sd, sampler.put()));
    }

    /// Draws the window's w x h picture from src into the slot, as large as fits, on black
    bool draw_scaled(int slot, ID3D11Texture2D *src, const D3D11_TEXTURE2D_DESC &desc, UINT w, UINT h) {
      if (!scaler_tried) {
        scaler_tried = true;
        scaler_ok = make_scaler();
      }
      if (!scaler_ok || w == 0 || h == 0) {
        return false;
      }
      if (!picture || picture_desc.Width != w || picture_desc.Height != h || picture_desc.Format != desc.Format) {
        picture_view = nullptr;
        picture = nullptr;
        D3D11_TEXTURE2D_DESC t {};
        t.Width = w;
        t.Height = h;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.Format = desc.Format;
        t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&t, nullptr, picture.put())) ||
            FAILED(device->CreateShaderResourceView(picture.get(), nullptr, picture_view.put()))) {
          picture = nullptr;
          picture_view = nullptr;
          return false;
        }
        picture_desc = t;
      }
      D3D11_BOX box {0, 0, 0, w, h, 1};
      ctx->CopySubresourceRegion(picture.get(), 0, 0, 0, 0, src, 0, &box);

      const float scale = (std::min)((float) ring_desc.Width / w, (float) ring_desc.Height / h);
      const float dw = (std::min)((float) ring_desc.Width, std::round(w * scale));
      const float dh = (std::min)((float) ring_desc.Height, std::round(h * scale));
      const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
      ctx->ClearRenderTargetView(targets[slot].get(), black);
      D3D11_VIEWPORT vp {std::floor((ring_desc.Width - dw) / 2), std::floor((ring_desc.Height - dh) / 2), dw, dh, 0.0f, 1.0f};
      ctx->RSSetViewports(1, &vp);
      ctx->RSSetState(nullptr);
      ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
      ID3D11RenderTargetView *rtv = targets[slot].get();
      ctx->OMSetRenderTargets(1, &rtv, nullptr);
      ctx->IASetInputLayout(nullptr);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(vs.get(), nullptr, 0);
      ctx->PSSetShader(ps.get(), nullptr, 0);
      ID3D11ShaderResourceView *srv = picture_view.get();
      ctx->PSSetShaderResources(0, 1, &srv);
      ID3D11SamplerState *smp = sampler.get();
      ctx->PSSetSamplers(0, 1, &smp);
      ctx->Draw(3, 0);
      // Let go of the shared texture before handing it over
      ID3D11ShaderResourceView *no_srv = nullptr;
      ctx->PSSetShaderResources(0, 1, &no_srv);
      ctx->OMSetRenderTargets(0, nullptr, nullptr);
      return true;
    }

    bool send(const msg_t &m) {
      DWORD written = 0;
      return WriteFile(frame_pipe, &m, sizeof(m), &written, nullptr) && written == sizeof(m);
    }

    void quit(int code) {
      TerminateProcess(GetCurrentProcess(), (UINT) code);
    }

    /// Sends the last frame again if nothing new came for a second (called every half second).
    void repeat_if_still() {
      std::lock_guard lg {lock};
      if (last_slot < 0 || GetTickCount64() - last_sent_ms < 1000) {
        return;
      }
      LARGE_INTEGER now {};
      LARGE_INTEGER freq {};
      QueryPerformanceCounter(&now);
      QueryPerformanceFrequency(&freq);
      msg_t m {};
      m.magic = MAGIC;
      m.type = MSG_FRAME;
      m.slot = (std::uint32_t) last_slot;
      m.width = ring_desc.Width;
      m.height = ring_desc.Height;
      m.format = (std::uint32_t) ring_desc.Format;
      // In the units of SystemRelativeTime: 100 ns
      m.qpc = (std::uint64_t) (now.QuadPart / freq.QuadPart * 10'000'000 + now.QuadPart % freq.QuadPart * 10'000'000 / freq.QuadPart);
      last_sent_ms = GetTickCount64();
      if (!send(m)) {
        quit(0);  // TunaStream went away
      }
    }

    bool make_ring(const D3D11_TEXTURE2D_DESC &src) {
      for (int i = 0; i < SLOTS; ++i) {
        targets[i] = nullptr;
        mutexes[i] = nullptr;
        ring[i] = nullptr;
        if (handles[i]) {
          CloseHandle(handles[i]);
          handles[i] = nullptr;
        }
      }
      D3D11_TEXTURE2D_DESC t {};
      t.Width = window_mode ? ring_width : src.Width;
      t.Height = window_mode ? ring_height : src.Height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_DEFAULT;
      t.Format = src.Format;
      t.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
      t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

      msg_t m {};
      m.magic = MAGIC;
      m.type = MSG_RING;
      m.width = t.Width;
      m.height = t.Height;
      m.format = (std::uint32_t) t.Format;
      for (int i = 0; i < SLOTS; ++i) {
        if (FAILED(device->CreateTexture2D(&t, nullptr, ring[i].put()))) {
          return false;
        }
        if (FAILED(ring[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), mutexes[i].put_void()))) {
          return false;
        }
        if (window_mode && FAILED(device->CreateRenderTargetView(ring[i].get(), nullptr, targets[i].put()))) {
          return false;
        }
        winrt::com_ptr<IDXGIResource1> resource;
        if (FAILED(ring[i]->QueryInterface(__uuidof(IDXGIResource1), resource.put_void())) ||
            FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handles[i]))) {
          return false;
        }
        m.handles[i] = (std::uint64_t) (std::uintptr_t) handles[i];
      }
      ring_desc = t;
      next_slot = 0;
      return send(m);
    }

    void on_frame(winrt::Direct3D11CaptureFramePool const &sender) {
      winrt::Direct3D11CaptureFrame frame {nullptr};
      try {
        frame = sender.TryGetNextFrame();
      } catch (winrt::hresult_error &) {
        return;
      }
      if (frame == nullptr) {
        return;
      }
      std::lock_guard lg {lock};
      auto content = frame.ContentSize();
      if (content.Width != size.Width || content.Height != size.Height) {
        if (!window_mode) {
          quit(EXIT_DISPLAY_CHANGED);
        }
        // The window changed size: the capture follows, from the next frame
        size = content;
        try {
          sender.Recreate(uwp_device, pixel_format, 2, size);
        } catch (winrt::hresult_error &) {
          quit(EXIT_WINDOW_CLOSED);
        }
        return;
      }

      winrt::com_ptr<ID3D11Texture2D> src;
      auto access = frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
      if (!access || FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), src.put_void()))) {
        return;
      }
      D3D11_TEXTURE2D_DESC desc;
      src->GetDesc(&desc);
      const bool size_differs = window_mode ? false : (desc.Width != ring_desc.Width || desc.Height != ring_desc.Height);
      if (!ring[0] || size_differs || desc.Format != ring_desc.Format) {
        if (!make_ring(desc)) {
          quit(8);
        }
      }

      // Use the next texture TunaStream isn't reading from
      for (int k = 0; k < SLOTS; ++k) {
        const int slot = (next_slot + k) % SLOTS;
        if (mutexes[slot]->AcquireSync(0, 0) != S_OK) {
          continue;
        }
        if (window_mode) {
          const UINT cw = (std::min)((UINT) (std::max)(0, content.Width), desc.Width);
          const UINT ch = (std::min)((UINT) (std::max)(0, content.Height), desc.Height);
          if (!draw_scaled(slot, src.get(), desc, cw, ch)) {
            // Couldn't size it: the window in the middle, on black; anything bigger than the display is cut off
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            ctx->ClearRenderTargetView(targets[slot].get(), black);
            const UINT w = (std::min)(cw, ring_desc.Width);
            const UINT h = (std::min)(ch, ring_desc.Height);
            D3D11_BOX box {0, 0, 0, w, h, 1};
            ctx->CopySubresourceRegion(ring[slot].get(), 0, (ring_desc.Width - w) / 2, (ring_desc.Height - h) / 2, 0, src.get(), 0, &box);
          }
        } else {
          ctx->CopyResource(ring[slot].get(), src.get());
        }
        mutexes[slot]->ReleaseSync(0);
        next_slot = (slot + 1) % SLOTS;
        last_slot = slot;
        last_sent_ms = GetTickCount64();

        msg_t m {};
        m.magic = MAGIC;
        m.type = MSG_FRAME;
        m.slot = (std::uint32_t) slot;
        m.width = ring_desc.Width;
        m.height = ring_desc.Height;
        m.format = (std::uint32_t) ring_desc.Format;
        m.qpc = (std::uint64_t) frame.SystemRelativeTime().count();
        if (!send(m)) {
          quit(0);  // TunaStream went away
        }
        break;
      }
    }
  };

  HANDLE open_pipe(const std::string &name, DWORD access) {
    const auto path = platf::from_utf8("\\\\.\\pipe\\" + name);
    for (int attempt = 0; attempt < 50; ++attempt) {
      HANDLE h = CreateFileW(path.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        return h;
      }
      if (GetLastError() == ERROR_PIPE_BUSY) {
        WaitNamedPipeW(path.c_str(), 100);
      } else {
        Sleep(100);
      }
    }
    return INVALID_HANDLE_VALUE;
  }
}  // namespace

namespace platf {
  int wgc_helper_main(int argc, char *argv[]) {
    using namespace platf::dxgi::wgc_ipc;
    // tunastream.exe --wgc-helper <pipe> <luid low> <luid high> <display> <hdr> <fps>
    //                [window <hwnd> <display width> <display height>]
    if (argc < 8) {
      return 2;
    }
    HWND window = nullptr;
    UINT window_ring_w = 0;
    UINT window_ring_h = 0;
    if (argc >= 12 && std::string {argv[8]} == "window") {
      window = (HWND) (std::uintptr_t) std::strtoull(argv[9], nullptr, 10);
      window_ring_w = (UINT) std::strtoul(argv[10], nullptr, 10);
      window_ring_h = (UINT) std::strtoul(argv[11], nullptr, 10);
      if (!IsWindow(window) || window_ring_w == 0 || window_ring_h == 0) {
        return EXIT_WINDOW_CLOSED;
      }
      // A minimized window has no picture to capture (only black): it's put back on the screen,
      // without taking the keyboard from whatever is in front. A window that's covered keeps
      // drawing in most programs; browsers may stop, so it's brought up once, above the others.
      if (IsIconic(window)) {
        ShowWindow(window, SW_SHOWNOACTIVATE);
      }
      SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    const std::string pipe = argv[2];
    LUID luid;
    luid.LowPart = (DWORD) std::strtoul(argv[3], nullptr, 10);
    luid.HighPart = (LONG) std::strtol(argv[4], nullptr, 10);
    const std::wstring display_name = from_utf8(argv[5]);
    const bool hdr = argv[6][0] == '1';
    const int fps = (std::max)(1, std::atoi(argv[7]));

    helper_t helper;
    helper.frame_pipe = open_pipe(pipe + "-f", GENERIC_WRITE);
    helper.ctrl_pipe = open_pipe(pipe + "-c", GENERIC_READ);
    if (helper.frame_pipe == INVALID_HANDLE_VALUE || helper.ctrl_pipe == INVALID_HANDLE_VALUE) {
      return 3;
    }

    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error &) {
      // already initialised
    }

    // The same graphics card TunaStream encodes on
    winrt::com_ptr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()))) {
      return 4;
    }
    winrt::com_ptr<IDXGIAdapter1> adapter;
    for (UINT i = 0;; ++i) {
      winrt::com_ptr<IDXGIAdapter1> candidate;
      if (factory->EnumAdapters1(i, candidate.put()) == DXGI_ERROR_NOT_FOUND) {
        break;
      }
      DXGI_ADAPTER_DESC1 desc;
      if (SUCCEEDED(candidate->GetDesc1(&desc)) && desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) {
        adapter = candidate;
        break;
      }
    }
    if (!adapter) {
      return 4;
    }
    if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, helper.device.put(), nullptr, helper.ctx.put()))) {
      return 4;
    }

    // The display to capture, by its name (\\.\DISPLAYn)
    struct find_t {
      std::wstring name;
      HMONITOR found = nullptr;
    } find {display_name};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
      auto f = (find_t *) param;
      MONITORINFOEXW info {};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(monitor, &info) && f->name == info.szDevice) {
        f->found = monitor;
        return FALSE;
      }
      return TRUE;
    }, (LPARAM) &find);
    if (!find.found && !window) {
      return 5;
    }

    winrt::GraphicsCaptureItem item {nullptr};
    winrt::Direct3D11CaptureFramePool pool {nullptr};
    winrt::GraphicsCaptureSession session {nullptr};
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        return 6;
      }
      winrt::com_ptr<IDXGIDevice> dxgi_device;
      winrt::com_ptr<::IInspectable> inspectable;
      if (FAILED(helper.device->QueryInterface(__uuidof(IDXGIDevice), dxgi_device.put_void())) ||
          FAILED(winrt::CreateDirect3D11DeviceFromDXGIDevice(dxgi_device.get(), inspectable.put()))) {
        return 6;
      }
      auto uwp_device = inspectable.as<winrt::IDirect3DDevice>();
      auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      if (!interop) {
        return 6;
      }
      if (window) {
        if (FAILED(interop->CreateForWindow(window, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
          return EXIT_WINDOW_CLOSED;
        }
        helper.window_mode = true;
        helper.ring_width = window_ring_w;
        helper.ring_height = window_ring_h;
        item.Closed([&helper](auto &&, auto &&) {
          helper.quit(EXIT_WINDOW_CLOSED);
        });
      } else if (FAILED(interop->CreateForMonitor(find.found, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
        return 6;
      }
      helper.size = item.Size();
      auto format = hdr ? winrt::Windows::Graphics::DirectX::DirectXPixelFormat::R16G16B16A16Float : winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
      helper.uwp_device = uwp_device;
      helper.pixel_format = format;
      pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, format, 2, helper.size);
      session = pool.CreateCaptureSession(item);
      pool.FrameArrived([&helper](winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
        helper.on_frame(sender);
      });
    } catch (winrt::hresult_error &) {
      return 7;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        session.IsBorderRequired(false);
      }
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        session.MinUpdateInterval(winrt::TimeSpan {10000000 / (fps * 2)});
      }
    } catch (winrt::hresult_error &) {
      // optional
    }
    try {
      session.StartCapture();
    } catch (winrt::hresult_error &) {
      return 7;
    }
    {
      msg_t m {};
      m.magic = MAGIC;
      m.type = MSG_READY;
      std::lock_guard lg {helper.lock};
      if (!helper.send(m)) {
        return 0;
      }
    }

    // A window that sits still: its last frame again every second
    std::thread repeater;
    if (window) {
      repeater = std::thread([&helper]() {
        while (true) {
          Sleep(500);
          helper.repeat_if_still();
        }
      });
      repeater.detach();
    }

    // Cursor on/off until TunaStream closes the pipe
    while (true) {
      char c = 0;
      DWORD got = 0;
      if (!ReadFile(helper.ctrl_pipe, &c, 1, &got, nullptr) || got == 0) {
        break;
      }
      try {
        if (c == 'C' || c == 'c') {
          session.IsCursorCaptureEnabled(c == 'C');
        }
      } catch (winrt::hresult_error &) {
      }
    }
    helper.quit(0);
    return 0;
  }
}  // namespace platf

namespace platf {
  namespace {
    std::atomic<std::uint64_t> window_target {0};
  }

  void set_window_capture(std::uint64_t hwnd) {
    window_target = hwnd;
  }

  std::uint64_t window_capture() {
    return window_target.load();
  }
}  // namespace platf

namespace platf::dxgi {
  namespace {
    // After the helper fails, DDX is used for a while before WGC is tried again
    std::atomic<std::int64_t> wgc_retry_after {0};

    // Display changes restart the helper; too many in a row means something else is wrong
    std::atomic<std::int64_t> display_change_window {0};
    std::atomic<int> display_changes {0};

    std::int64_t steady_ms() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    const char *helper_exit_reason(DWORD code) {
      switch (code) {
        case 3:
          return "couldn't connect";
        case 4:
          return "couldn't open the graphics card";
        case 5:
          return "couldn't find the display";
        case 6:
          return "WGC isn't available";
        case 7:
          return "couldn't start capturing";
        case 8:
          return "couldn't share frames";
        case wgc_ipc::EXIT_DISPLAY_CHANGED:
          return "the display changed";
        case wgc_ipc::EXIT_WINDOW_CLOSED:
          return "the window closed";
        case STILL_ACTIVE:
          return "stopped answering";
        default:
          return "stopped";
      }
    }
  }  // namespace

  bool secure_desktop_active() {
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desk) {
      return true;  // can't tell: DDX copes with either
    }
    wchar_t name[64] {};
    DWORD needed = 0;
    bool secure = false;
    if (GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed)) {
      secure = _wcsicmp(name, L"Default") != 0;
    }
    CloseDesktop(desk);
    return secure;
  }

  class wgc_remote_t {
  public:
    ~wgc_remote_t() {
      release_frame();
      if (ctrl_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(ctrl_pipe);  // the helper quits when this closes
        ctrl_pipe = INVALID_HANDLE_VALUE;
      }
      if (frame_pipe != INVALID_HANDLE_VALUE) {
        CancelIoEx(frame_pipe, nullptr);
      }
      if (reader.joinable()) {
        reader.join();
      }
      if (process) {
        if (WaitForSingleObject(process, 1000) != WAIT_OBJECT_0) {
          TerminateProcess(process, 0);
        }
        CloseHandle(process);
      }
      if (frame_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(frame_pipe);
      }
    }

    int start(display_base_t *d, const ::video::config_t &config) {
      display = d;
      window = window_capture();
      DXGI_ADAPTER_DESC1 adapter_desc;
      DXGI_OUTPUT_DESC output_desc;
      if (FAILED(d->adapter->GetDesc1(&adapter_desc)) || FAILED(d->output->GetDesc(&output_desc))) {
        return -1;
      }
      d->capture_format = config.dynamicRange ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;

      HANDLE token = retrieve_users_token(false);
      if (!token) {
        BOOST_LOG(info) << "WGC: nobody is signed in, using DDX"sv;
        return -1;
      }

      // Pipes only SYSTEM, administrators and the signed-in user can open, with a random name
      std::random_device rd;
      std::string pipe = "TunaStreamWGC-";
      for (int i = 0; i < 4; ++i) {
        char part[9];
        std::snprintf(part, sizeof(part), "%08x", (unsigned) rd());
        pipe += part;
      }
      PSECURITY_DESCRIPTOR sd = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)", SDDL_REVISION_1, &sd, nullptr)) {
        CloseHandle(token);
        return -1;
      }
      SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
      auto make_pipe = [&](const std::string &name, DWORD direction) {
        return CreateNamedPipeW(from_utf8("\\\\.\\pipe\\" + name).c_str(), direction | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024, 64 * 1024, 0, &sa);
      };
      frame_pipe = make_pipe(pipe + "-f", PIPE_ACCESS_INBOUND);
      ctrl_pipe = make_pipe(pipe + "-c", PIPE_ACCESS_OUTBOUND);
      LocalFree(sd);
      if (frame_pipe == INVALID_HANDLE_VALUE || ctrl_pipe == INVALID_HANDLE_VALUE) {
        BOOST_LOG(warning) << "WGC: couldn't create the helper's pipes: "sv << GetLastError();
        CloseHandle(token);
        return -1;
      }

      // Start the helper: this same program, as the user, on the user's desktop
      std::wstring exe(32768, L'\0');
      exe.resize(GetModuleFileNameW(nullptr, exe.data(), (DWORD) exe.size()));
      const auto dir = std::filesystem::path {exe}.parent_path().wstring();
      std::wstring cmd = L"\"" + exe + L"\" --wgc-helper " + from_utf8(pipe) + L" " +
                         std::to_wstring(adapter_desc.AdapterLuid.LowPart) + L" " + std::to_wstring(adapter_desc.AdapterLuid.HighPart) +
                         L" \"" + output_desc.DeviceName + L"\" " + (config.dynamicRange ? L"1" : L"0") + L" " + std::to_wstring(config.framerate);
      if (window) {
        // Only this window, in the middle of a picture the size of its display
        cmd += L" window " + std::to_wstring(window) + L" " + std::to_wstring(d->width_before_rotation) + L" " + std::to_wstring(d->height_before_rotation);
      }
      STARTUPINFOW si {};
      si.cb = sizeof(si);
      wchar_t desktop[] = L"winsta0\\default";
      si.lpDesktop = desktop;
      void *env = nullptr;
      CreateEnvironmentBlock(&env, token, FALSE);
      PROCESS_INFORMATION pi {};
      const BOOL started = CreateProcessAsUserW(token, exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env, dir.c_str(), &si, &pi);
      const DWORD start_error = GetLastError();
      if (env) {
        DestroyEnvironmentBlock(env);
      }
      CloseHandle(token);
      if (!started) {
        BOOST_LOG(warning) << "WGC: couldn't start the capture helper: "sv << start_error;
        return -1;
      }
      CloseHandle(pi.hThread);
      process = pi.hProcess;

      if (!connect(frame_pipe) || !connect(ctrl_pipe)) {
        BOOST_LOG(warning) << "WGC: the capture helper didn't connect ("sv << exit_reason() << ')';
        return -1;
      }
      ULONG client_pid = 0;
      if (!GetNamedPipeClientProcessId(frame_pipe, &client_pid) || client_pid != pi.dwProcessId ||
          !GetNamedPipeClientProcessId(ctrl_pipe, &client_pid) || client_pid != pi.dwProcessId) {
        BOOST_LOG(warning) << "WGC: an unexpected program connected to the capture helper's pipe"sv;
        return -1;
      }

      reader = std::thread {[this]() {
        read_loop();
      }};
      std::unique_lock lk {lock};
      if (!cv.wait_for(lk, 5s, [this]() {
            return ready || dead;
          }) ||
          dead) {
        lk.unlock();
        BOOST_LOG(warning) << "WGC: the capture helper didn't start ("sv << exit_reason() << ')';
        return -1;
      }
      BOOST_LOG(info) << "WGC: capturing through the helper in the user's session"sv;
      return 0;
    }

    capture_e next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, std::uint64_t &out_time) {
      release_frame();

      const auto now = std::chrono::steady_clock::now();
      // A window keeps its own picture whatever desktop shows; never switch to the whole screen
      if (!window && now >= next_desktop_check) {
        next_desktop_check = now + 250ms;
        if (secure_desktop_active()) {
          BOOST_LOG(info) << "WGC: the secure desktop is showing, switching to DDX until it closes"sv;
          return capture_e::reinit;
        }
      }

      std::unique_lock lk {lock};
      if (!cv.wait_for(lk, timeout, [this]() {
            return dead || seq != consumed_seq;
          })) {
        return capture_e::timeout;
      }
      if (dead) {
        lk.unlock();
        auto code = exit_code();
        const auto now_ms = steady_ms();
        if (now_ms - display_change_window.load() > 30000) {
          display_change_window = now_ms;
          display_changes = 0;
        }
        if (window) {
          BOOST_LOG(info) << "WGC: the shared window's capture stopped ("sv << helper_exit_reason(code) << ')';
        } else if (code == wgc_ipc::EXIT_DISPLAY_CHANGED && ++display_changes <= 3) {
          BOOST_LOG(info) << "WGC: the display changed, capturing it again"sv;
        } else {
          BOOST_LOG(warning) << "WGC: the capture helper "sv << helper_exit_reason(code) << ", using DDX for now"sv;
          wgc_retry_after = steady_ms() + 60000;
        }
        return capture_e::reinit;
      }
      consumed_seq = seq;
      const int slot = latest_slot;
      const std::uint64_t qpc = latest_qpc;
      const std::uint32_t gen = ring_gen;
      std::uint64_t handles[wgc_ipc::SLOTS];
      std::copy_n(ring_handles, wgc_ipc::SLOTS, handles);
      lk.unlock();

      if (slot < 0) {
        return capture_e::timeout;
      }
      if (gen != opened_gen) {
        if (open_ring(handles)) {
          wgc_retry_after = steady_ms() + 60000;
          return capture_e::reinit;
        }
        opened_gen = gen;
      }

      if (mutexes[slot]->AcquireSync(0, 100) != S_OK) {
        return capture_e::timeout;
      }
      held = slot;
      *out = ring[slot].get();
      (*out)->AddRef();
      out_time = qpc;
      return capture_e::ok;
    }

    capture_e release_frame() {
      if (held >= 0) {
        mutexes[held]->ReleaseSync(0);
        held = -1;
      }
      return capture_e::ok;
    }

    void set_cursor_visible(bool visible) {
      const int want = visible ? 1 : 0;
      if (cursor == want || ctrl_pipe == INVALID_HANDLE_VALUE) {
        return;
      }
      const char c = visible ? 'C' : 'c';
      OVERLAPPED ov {};
      ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      DWORD written = 0;
      BOOL ok = WriteFile(ctrl_pipe, &c, 1, nullptr, &ov);
      if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 500) != WAIT_OBJECT_0) {
          CancelIoEx(ctrl_pipe, &ov);
        }
        ok = GetOverlappedResult(ctrl_pipe, &ov, &written, TRUE);
      }
      CloseHandle(ov.hEvent);
      if (ok) {
        cursor = want;
      }
    }

  private:
    display_base_t *display = nullptr;
    std::uint64_t window = 0;  ///< one window instead of the whole display
    HANDLE process = nullptr;
    HANDLE frame_pipe = INVALID_HANDLE_VALUE;
    HANDLE ctrl_pipe = INVALID_HANDLE_VALUE;
    std::thread reader;

    // Filled in by the reader thread
    std::mutex lock;
    std::condition_variable cv;
    bool ready = false;
    bool dead = false;
    std::uint64_t seq = 0;
    int latest_slot = -1;
    std::uint64_t latest_qpc = 0;
    std::uint32_t ring_gen = 0;
    std::uint64_t ring_handles[wgc_ipc::SLOTS] {};

    // Used on the capture thread
    std::uint64_t consumed_seq = 0;
    std::uint32_t opened_gen = 0;
    texture2d_t ring[wgc_ipc::SLOTS];
    keyed_mutex_t mutexes[wgc_ipc::SLOTS];
    int held = -1;
    int cursor = -1;
    std::chrono::steady_clock::time_point next_desktop_check {};

    bool connect(HANDLE pipe) {
      OVERLAPPED ov {};
      ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      bool ok = false;
      if (ConnectNamedPipe(pipe, &ov)) {
        ok = true;
      } else if (GetLastError() == ERROR_PIPE_CONNECTED) {
        ok = true;
      } else if (GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[2] = {ov.hEvent, process};
        DWORD transferred = 0;
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) == WAIT_OBJECT_0) {
          ok = GetOverlappedResult(pipe, &ov, &transferred, FALSE);
        } else {
          CancelIoEx(pipe, &ov);
          GetOverlappedResult(pipe, &ov, &transferred, TRUE);
        }
      }
      CloseHandle(ov.hEvent);
      return ok;
    }

    DWORD exit_code() {
      DWORD code = STILL_ACTIVE;
      if (process) {
        WaitForSingleObject(process, 500);
        GetExitCodeProcess(process, &code);
      }
      return code;
    }

    const char *exit_reason() {
      return helper_exit_reason(exit_code());
    }

    void read_loop() {
      OVERLAPPED ov {};
      ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      while (true) {
        wgc_ipc::msg_t m {};
        DWORD got = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(frame_pipe, &m, sizeof(m), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
          break;
        }
        if (!GetOverlappedResult(frame_pipe, &ov, &got, TRUE) || got != sizeof(m) || m.magic != wgc_ipc::MAGIC) {
          break;
        }
        std::lock_guard lg {lock};
        if (m.type == wgc_ipc::MSG_READY) {
          ready = true;
        } else if (m.type == wgc_ipc::MSG_RING) {
          ++ring_gen;
          std::copy_n(m.handles, wgc_ipc::SLOTS, ring_handles);
          latest_slot = -1;  // frames from here on are in the new textures
        } else if (m.type == wgc_ipc::MSG_FRAME && m.slot < (std::uint32_t) wgc_ipc::SLOTS) {
          latest_slot = (int) m.slot;
          latest_qpc = m.qpc;
          ++seq;
        }
        cv.notify_all();
      }
      CloseHandle(ov.hEvent);
      std::lock_guard lg {lock};
      dead = true;
      cv.notify_all();
    }

    int open_ring(const std::uint64_t handles[wgc_ipc::SLOTS]) {
      release_frame();
      util::safe_ptr<ID3D11Device1, Release<ID3D11Device1>> device1;
      if (FAILED(display->device->QueryInterface(__uuidof(ID3D11Device1), (void **) &device1))) {
        return -1;
      }
      for (int i = 0; i < wgc_ipc::SLOTS; ++i) {
        ring[i].reset();
        mutexes[i].reset();
      }
      for (int i = 0; i < wgc_ipc::SLOTS; ++i) {
        HANDLE mine = nullptr;
        if (!DuplicateHandle(process, (HANDLE) (std::uintptr_t) handles[i], GetCurrentProcess(), &mine, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
          BOOST_LOG(warning) << "WGC: couldn't take the helper's frames: "sv << GetLastError();
          return -1;
        }
        auto status = device1->OpenSharedResource1(mine, __uuidof(ID3D11Texture2D), (void **) &ring[i]);
        CloseHandle(mine);
        if (FAILED(status) || FAILED(ring[i]->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **) &mutexes[i]))) {
          BOOST_LOG(warning) << "WGC: couldn't open the helper's frames [0x"sv << util::hex(status).to_string_view() << ']';
          return -1;
        }
      }
      return 0;
    }
  };

  std::shared_ptr<wgc_remote_t> make_wgc_remote(display_base_t *display, const ::video::config_t &config) {
    if (window_capture()) {
      // Sharing one window: always through the helper, never the whole screen instead
      auto remote = std::make_shared<wgc_remote_t>();
      return remote->start(display, config) ? nullptr : remote;
    }
    if (steady_ms() < wgc_retry_after.load()) {
      return nullptr;
    }
    if (secure_desktop_active()) {
      BOOST_LOG(info) << "WGC: the secure desktop is showing, using DDX until it closes"sv;
      return nullptr;
    }
    auto remote = std::make_shared<wgc_remote_t>();
    if (remote->start(display, config)) {
      wgc_retry_after = steady_ms() + 60000;
      return nullptr;
    }
    return remote;
  }

  capture_e wgc_remote_next_frame(wgc_remote_t &remote, std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    return remote.next_frame(timeout, out, out_time);
  }

  capture_e wgc_remote_release_frame(wgc_remote_t &remote) {
    return remote.release_frame();
  }

  void wgc_remote_set_cursor_visible(wgc_remote_t &remote, bool visible) {
    remote.set_cursor_visible(visible);
  }
}  // namespace platf::dxgi
