#pragma once

#include "SdlRenderManager.h"
#include "../RenderManager.h"
#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// SDL_GPU (command-buffer) in-engine render backend. Registered as
// "sdl3_gpu". Compositing operations are recorded onto a per-frame SDL_GPU
// command buffer and submitted at the end of the frame. Operations that are
// not yet implemented on the GPU path fall back to the software delegate so
// behavior is always correct (just slower for those ops).
//
// Mirrors krkrsdl3's software/GL dual RenderManager design: this manager is
// selected only when the host injects an SDL_GPUDevice via
// engine_set_sdl_gpu_device and the renderer option is "sdl3_gpu".

class SdlGpuTexture2D final : public iTVPTexture2D {
public:
    SdlGpuTexture2D(const void *pixel, int pitch, unsigned int w,
                    unsigned int h, TVPTextureFormat::e format,
                    int create_flags = RENDER_CREATE_TEXTURE_FLAG_ANY);
    ~SdlGpuTexture2D() override;

    TVPTextureFormat::e GetFormat() const override { return format_; }
    const void *GetScanLineForRead(tjs_uint l) override;
    void *GetScanLineForWrite(tjs_uint l) override;
    tjs_int GetPitch() const override { return pitch_; }
    void Update(const void *pixel, TVPTextureFormat::e format, int pitch,
                const tTVPRect &rc) override;
    uint32_t GetPoint(int x, int y) override;
    void SetPoint(int x, int y, uint32_t clr) override;
    void SetSize(unsigned int w, unsigned int h) override;
    bool IsStatic() override { return false; }
    bool IsOpaque() override { return opacity_known_ && opaque_; }
    bool HasKnownTransparency() const { return opacity_known_ && !opaque_; }
    krkr::Texture2D *GetAdapterTexture(krkr::Texture2D *origTex) override {
        return origTex;
    }

    SDL_GPUTexture *GpuTexture() const { return gpu_tex_; }
    bool EnsureGpuTexture();
    void UploadCpuToGpu();
    bool EnsureCpuReadable();
    void MarkCpuDirty() { cpu_dirty_ = true; }
    void MarkGpuDirty() { gpu_dirty_ = true; cpu_dirty_ = false; }
    bool HasPendingGpu() const { return gpu_dirty_ && !cpu_dirty_; }
    bool RequiresGpuReadback() const {
        return gpu_tex_ != nullptr && !cpu_dirty_ && pixels_.empty();
    }
    void SetOpacityKnown(bool opaque);

private:
    void ReleaseGpuTexture();
    void EnsureCpuStorage();
    void DiscardCpuStorage();

    TVPTextureFormat::e format_ = TVPTextureFormat::RGBA;
    int pitch_ = 0;
    std::vector<uint8_t> pixels_;
    SDL_GPUTexture *gpu_tex_ = nullptr;
    bool gpu_dirty_ = false;
    bool cpu_dirty_ = false;
    bool opacity_known_ = false;
    bool opaque_ = false;
};

class SdlGpuRenderManager final : public iTVPRenderManager {
public:
    SdlGpuRenderManager() = default;

    iTVPTexture2D *CreateTexture2D(const void *pixel, int pitch, unsigned int w,
                                   unsigned int h, TVPTextureFormat::e format,
                                   int flags = RENDER_CREATE_TEXTURE_FLAG_ANY) override;
    iTVPTexture2D *CreateTexture2D(tTVPBitmap *bmp) override;
    iTVPTexture2D *CreateTexture2D(TJS::tTJSBinaryStream *s) override;
    iTVPTexture2D *CreateTexture2D(unsigned int neww, unsigned int newh,
                                   iTVPTexture2D *tex) override;

    iTVPRenderMethod *GetRenderMethod(const char *name,
                                      uint32_t *hint = nullptr) override;
    const char *GetName() override { return "SdlGpu"; }
    bool GetRenderStat(unsigned int &drawCount, uint64_t &vmemsize) override;
    bool GetTextureStat(iTVPTexture2D *texture, uint64_t &vmemsize) override;
    int EnumParameterID(const char *name) override;
    void SetParameterUInt(int id, unsigned int Value) override;
    void SetParameterInt(int id, int Value) override;
    void SetParameterPtr(int id, const void *Value) override;
    void SetParameterFloat(int id, float Value) override;

    void OperateRect(iTVPRenderMethod *method, iTVPTexture2D *tar,
                     iTVPTexture2D *reftar, const tTVPRect &rctar,
                     const tRenderTexRectArray &textures) override;
    void OperateTriangles(iTVPRenderMethod *method, int nTriangles,
                          iTVPTexture2D *target, iTVPTexture2D *reftar,
                          const tTVPRect &rcclip,
                          const tTVPPointD *pttar,
                          const tRenderTexQuadArray &textures) override;
    void OperatePerspective(iTVPRenderMethod *method, int nQuads,
                            iTVPTexture2D *target, iTVPTexture2D *reftar,
                            const tTVPRect &rcclip,
                            const tTVPPointD *pttar,
                            const tRenderTexQuadArray &textures) override;
    bool IsSoftware() override { return false; }

    // The engine calls these from the frame loop so the manager can drive the
    // SDL_GPU command buffer lifecycle.
    static void BeginComposite();
    static void EndComposite();

private:
    iTVPRenderManager *SoftwareDelegate();
    bool DrawRect(SdlGpuTexture2D *dst, const tTVPRect &rctar,
                  const char *method_name, uint32_t mode, int opacity,
                  uint32_t color, SDL_GPUTexture *src,
                  const tTVPRect &src_rc, int src_w, int src_h);

    iTVPRenderManager *software_delegate_ = nullptr;
    std::unordered_map<uint32_t, SDLRenderMethod *> method_wrappers_;
    unsigned int draw_count_ = 0;
    uint64_t vmem_size_ = 0;
    int stretch_parameter_id_ = -1;
    int stretch_type_ = 0;
};

void TVPForceRegisterSdlGpuRenderManager();
