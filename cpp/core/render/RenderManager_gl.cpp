#include "tjsCommHead.h"
#include "RenderManager.h"

#include "LayerBitmap.h"
#include "TVPMsg.h"
#include "tjsError.h"
#include "Platform.h"
#include "PlatformView.h"

#if _KRKRSDL3_GL
#include <glad/glad.h>
#else
#include <GLES3/gl3.h>
#endif

#if !_KRKRSDL3_GL
// OpenGL ES 3.x does not support these desktop-only GL features.
// Define as null so runtime feature checks evaluate to false.
#define GL_ALPHA_TEST 0
static constexpr auto glAlphaFunc = nullptr;
static constexpr auto glClearTexImage = nullptr;
static constexpr auto glClearTexSubImage = nullptr;
static constexpr auto glCopyImageSubData = nullptr;
#endif

#include "TVPImageUtils.h"

#include <map>
#include <set>

static std::set<GLenum> TVPTextureFormats;
void TVPInitTextureFormatList()
{
    if (TVPTextureFormats.empty())
    {
        GLint nTexFormats;
        glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, &nTexFormats);
        std::vector<GLint> texFormats;
        texFormats.resize(nTexFormats);
        glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, &texFormats.front());
        for (GLint f : texFormats)
        {
            TVPTextureFormats.insert(f);
        }
    }
}
bool TVPIsSupportTextureFormat(GLenum fmt)
{
    return TVPTextureFormats.end() != TVPTextureFormats.find(fmt);
}

class tTVPOGLTexture2D;
struct GLVertexInfo
{
    tTVPOGLTexture2D* tex;
    std::vector<GLfloat> vtx;
};

static bool _CurrentFBOValid = false;
static GLuint _FBO; // common frame buffer object for all renderable texture
static GLuint _stencil_FBO;
static GLuint _CurrentRenderTarget = 0;
static GLenum _glCompressedTexFormat = GL_RGBA;
unsigned int TVPMaxTextureSize;
static uint64_t _totalVMemSize = 0;
static unsigned int GetMaxTextureWidth()
{
    return TVPMaxTextureSize;
}
static unsigned int GetMaxTextureHeight()
{
    return TVPMaxTextureSize;
}
static unsigned int power_of_two(unsigned int input, unsigned int value = 32)
{
    while (value < input)
    {
        value <<= 1;
    }
    return value;
}

static void _glBindTexture2D(GLuint t)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, t);
}

static void _glBindTexture2DN(unsigned int i, GLuint t)
{
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, t);
}
enum VertexAttribFlag
{
    VERTEX_ATTRIB_FLAG_NONE = 0,
    VERTEX_ATTRIB_FLAG_POSITION = 1 << 0,   // 1
    VERTEX_ATTRIB_FLAG_COLOR = 1 << 1,      // 2
    VERTEX_ATTRIB_FLAG_TEX_COORDS = 1 << 2, // 4
    VERTEX_ATTRIB_FLAG_NORMAL = 1 << 3,     // 8
};
static unsigned int s_currentFlags = VERTEX_ATTRIB_FLAG_NONE;
static void _glEnableVertexAttribs(unsigned int flags)
{
    if (s_currentFlags == flags)
    {
        return;
    }

    const struct AttribMapping
    {
        unsigned int flag;
        GLuint index;
    } mappings[] = {
        {VERTEX_ATTRIB_FLAG_POSITION, 0},
        {VERTEX_ATTRIB_FLAG_COLOR, 1},
        {VERTEX_ATTRIB_FLAG_TEX_COORDS, 2},
        {VERTEX_ATTRIB_FLAG_NORMAL, 3}
    };
    const int mappingCount = sizeof(mappings) / sizeof(mappings[0]);

    for (int i = 0; i < mappingCount; ++i)
    {
        bool shouldEnable = (flags & mappings[i].flag) != 0;
        bool isCurrentlyEnabled = (s_currentFlags & mappings[i].flag) != 0;

        if (shouldEnable != isCurrentlyEnabled)
        {
            if (shouldEnable)
            {
                glEnableVertexAttribArray(mappings[i].index);
            }
            else
            {
                glDisableVertexAttribArray(mappings[i].index);
            }
        }
    }
    s_currentFlags = flags;
}

static GLint _prevRenderBuffer;
static GLint _screenFrameBuffer = 0;
static unsigned int _stencilBufferW = 0, _stencilBufferH = 0;
void TVPSetRenderTarget(GLuint t)
{
    if (t)
    {
        if (!_CurrentFBOValid)
        {
            _CurrentFBOValid = true;
            glGetIntegerv(GL_FRAMEBUFFER_BINDING, &_screenFrameBuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, _FBO);
        }
        if (_CurrentRenderTarget == t)
            return;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, _screenFrameBuffer);
        _CurrentFBOValid = false;
    }
    _CurrentRenderTarget = t;
}

static bool GL_CHECK_unpack_subimage = true;
static void _RestoreGLStatues()
{
    if (GL_CHECK_unpack_subimage)
    {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    glDisable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glBlendEquation(GL_FUNC_ADD);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GLsizei screenWidth;
    GLsizei screenHeight;
    TVPGetWindowSize(&screenWidth, &screenHeight);
    glViewport(0, 0, screenWidth, screenHeight);
}

static tjs_uint8* TVPShrinkXYBy2(
    tjs_uint* dpitch, const tjs_uint8* src, tjs_int spitch, tjs_uint srcw, tjs_uint srch)
{
    tjs_uint dstw = (srcw + 1) / 2, dsth = (srch + 1) / 2;
    tjs_uint dp = *dpitch = (dstw * 4 + 7) & ~7;
    tjs_uint8 *ret = new tjs_uint8[dp * dsth], *dst = ret;
    tjs_uint wpitch = srcw / 2 * 4, h = srch / 2;
    bool xtail = srcw & 1;
    for (tjs_uint y = 0; y < h; y++)
    {
        const tjs_uint8 *sline = src, *sline2 = src + spitch;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline[4] + sline2[0] + sline2[4];
            *dline++ = sum / 4;
            ++sline;
            ++sline2;
            sum = sline[0] + sline[4] + sline2[0] + sline2[4];
            *dline++ = sum / 4;
            ++sline;
            ++sline2;
            sum = sline[0] + sline[4] + sline2[0] + sline2[4];
            *dline++ = sum / 4;
            ++sline;
            ++sline2;
            sum = sline[0] + sline[4] + sline2[0] + sline2[4];
            *dline++ = sum / 4;
            ++sline;
            ++sline2;
            sline += 4;
            sline2 += 4;
        }
        if (xtail)
        {
            tjs_uint32 sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
            sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
            sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
            sum = sline[0] + sline2[0];
            *dline++ = sum / 2; /*++sline; ++sline2;*/
        }
        dst += dp;
        src += spitch;
        src += spitch;
    }
    if (srch & 1 /*ytail*/)
    {
        const tjs_uint8* sline = src;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sline += 4;
        }
        if (xtail)
        {
            tjs_uint32 sum = *(tjs_uint32*)sline;
            *(tjs_uint32*)dline = sum;
        }
    }
    return ret;
}

static tjs_uint8* TVPShrinkXBy2(
    tjs_uint* dpitch, const tjs_uint8* src, tjs_int spitch, tjs_uint srcw, tjs_uint srch)
{
    tjs_uint dstw = (srcw + 1) / 2;
    tjs_uint dp = *dpitch = (dstw * 4 + 7) & ~7;
    tjs_uint8 *ret = new tjs_uint8[dp * srch], *dst = ret;
    tjs_uint wpitch = srcw / 2 * 4;
    bool xtail = srcw & 1;
    for (tjs_uint y = 0; y < srch; y++)
    {
        const tjs_uint8* sline = src;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sum = sline[0] + sline[4];
            *dline++ = sum / 2;
            ++sline;
            sline += 4;
        }
        if (xtail)
        {
            tjs_uint32 sum = *(tjs_uint32*)sline;
            *(tjs_uint32*)dline = sum;
        }
        dst += dp;
        src += spitch;
    }
    return ret;
}

static tjs_uint8* TVPShrinkYBy2(
    tjs_uint* dpitch, const tjs_uint8* src, tjs_int spitch, tjs_uint srcw, tjs_uint srch)
{
    tjs_uint dsth = (srch + 1) / 2;
    tjs_uint dp = *dpitch = (srcw * 4 + 7) & ~7;
    tjs_uint8 *ret = new tjs_uint8[dp * dsth], *dst = ret;
    tjs_uint wpitch = srcw * 4, h = srch / 2;
    for (tjs_uint y = 0; y < h; y++)
    {
        const tjs_uint8 *sline = src, *sline2 = src + spitch;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
            sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
            sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
            sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
        }
        dst += dp;
        src += spitch;
        src += spitch;
    }
    if (srch & 1 /*ytail*/)
    {
        const tjs_uint8* sline = src;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = *(tjs_uint32*)sline;
            *(tjs_uint32*)dline = sum;
            dline += 4;
            sline += 4;
        }
    }
    return ret;
}

static tjs_uint8* TVPShrinkXYBy2_8(
    tjs_uint* dpitch, const tjs_uint8* src, tjs_int spitch, tjs_uint srcw, tjs_uint srch)
{
    tjs_uint dstw = (srcw + 1) / 2, dsth = (srch + 1) / 2;
    tjs_uint dp = *dpitch = (dstw + 7) & ~7;
    tjs_uint8 *ret = new tjs_uint8[dp * dsth], *dst = ret;
    tjs_uint wpitch = srcw / 2, h = srch / 2;
    bool xtail = srcw & 1;
    for (tjs_uint y = 0; y < h; y++)
    {
        const tjs_uint8 *sline = src, *sline2 = src + spitch;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline[1] + sline2[0] + sline2[1];
            *dline++ = sum / 4;
            ++sline;
            ++sline2;
            sline += 1;
            sline2 += 1;
        }
        if (xtail)
        {
            tjs_uint32 sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
        }
        dst += dp;
        src += spitch;
        src += spitch;
    }
    if (srch & 1 /*ytail*/)
    {
        const tjs_uint8* sline = src;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline[1];
            *dline++ = sum / 2;
            ++sline;
            ++sline;
        }
        if (xtail)
        {
            *dline = *sline;
        }
    }
    return ret;
}

static tjs_uint8* TVPShrinkXBy2_8(
    tjs_uint* dpitch, const tjs_uint8* src, tjs_int spitch, tjs_uint srcw, tjs_uint srch)
{
    tjs_uint dstw = (srcw + 1) / 2;
    tjs_uint dp = *dpitch = (dstw + 7) & ~7;
    tjs_uint8 *ret = new tjs_uint8[dp * srch], *dst = ret;
    tjs_uint wpitch = srcw / 2;
    bool xtail = srcw & 1;
    for (tjs_uint y = 0; y < srch; y++)
    {
        const tjs_uint8* sline = src;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline[1];
            *dline++ = sum / 2;
            ++sline;
            sline += 1;
        }
        if (xtail)
        {
            *dline = *sline;
        }
        dst += dp;
        src += spitch;
    }
    return ret;
}

static tjs_uint8* TVPShrinkYBy2_8(
    tjs_uint* dpitch, const tjs_uint8* src, tjs_int spitch, tjs_uint srcw, tjs_uint srch)
{
    tjs_uint dsth = (srch + 1) / 2;
    tjs_uint dp = *dpitch = (srcw + 7) & ~7;
    tjs_uint8 *ret = new tjs_uint8[dp * dsth], *dst = ret;
    tjs_uint wpitch = srcw, h = srch / 2;
    for (tjs_uint y = 0; y < h; y++)
    {
        const tjs_uint8 *sline = src, *sline2 = src + spitch;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            tjs_uint32 sum = sline[0] + sline2[0];
            *dline++ = sum / 2;
            ++sline;
            ++sline2;
        }
        dst += dp;
        src += spitch;
        src += spitch;
    }
    if (srch & 1 /*ytail*/)
    {
        const tjs_uint8* sline = src;
        tjs_uint8 *dline = dst, *lend = dline + wpitch;
        while (dline < lend)
        {
            *dline++ = *sline++;
        }
    }
    return ret;
}

static tjs_uint8* TVPShrink(tjs_uint* dpitch,
                            const tjs_uint8* src,
                            tjs_int spitch,
                            tjs_uint srcw,
                            tjs_uint srch,
                            tjs_uint dstw,
                            tjs_uint dsth)
{
    if (srcw == dstw)
    {
        if ((srch + 1) / 2 == dsth)
        {
            return TVPShrinkYBy2(dpitch, src, spitch, srcw, srch);
        }
    }
    else if ((srcw + 1) / 2 == dstw)
    {
        if (srch == dsth)
        {
            return TVPShrinkXBy2(dpitch, src, spitch, srcw, srch);
        }
        else if ((srch + 1) / 2 == dsth)
        {
            return TVPShrinkXYBy2(dpitch, src, spitch, srcw, srch);
        }
    }
    int newdpitch;
    tjs_uint8* tmp = TVPImageUtils::BilinearResizeRGBA(src, srcw, srch, spitch, dstw, dsth, newdpitch);
    *dpitch = newdpitch;
    return tmp;
}

static tjs_uint8* TVPShrink_8(tjs_uint* dpitch,
                              const tjs_uint8* src,
                              tjs_int spitch,
                              tjs_uint srcw,
                              tjs_uint srch,
                              tjs_uint dstw,
                              tjs_uint dsth)
{
    if (srcw == dstw)
    {
        if ((srch + 1) / 2 == dsth)
        {
            return TVPShrinkYBy2_8(dpitch, src, spitch, srcw, srch);
        }
    }
    else if ((srcw + 1) / 2 == dstw)
    {
        if (srch == dsth)
        {
            return TVPShrinkXBy2_8(dpitch, src, spitch, srcw, srch);
        }
        else if ((srch + 1) / 2 == dsth)
        {
            return TVPShrinkXYBy2_8(dpitch, src, spitch, srcw, srch);
        }
    }
    int newdpitch;
    tjs_uint8* tmp = TVPImageUtils::BilinearResize8(src, srcw, srch, spitch, dstw, dsth, newdpitch);
    *dpitch = newdpitch;
    return tmp;
}

static tjs_uint8* TVPShrinkImage(TVPTextureFormat::e fmt,
                                 tjs_uint& dpitch,
                                 const tjs_uint8* src,
                                 tjs_int spitch,
                                 tjs_uint srcw,
                                 tjs_uint srch,
                                 tjs_uint dstw,
                                 tjs_uint dsth)
{
    if (srch == dsth && srcw == dstw)
        return nullptr;
    return (fmt == TVPTextureFormat::RGBA ? TVPShrink : TVPShrink_8)(&dpitch, src, spitch, srcw,
                                                                     srch, dstw, dsth);
}

static bool TVPCheckOpaqueRGBA(const tjs_uint8* pixel, tjs_int pitch, tjs_int w, tjs_int h)
{
    const tjs_uint8* p = pixel;
    for (int y = 0; y < h; ++y)
    {
        const tjs_uint8* line = p;
        for (int x = 0; x < w; ++x)
        {
            if (line[3] != 0xFF)
            {
                return false;
            }
            line += 4;
        }
        p += pitch;
    }
    return true;
}

static bool TVPCheckSolidPixel(const tjs_uint8* pixel, tjs_int pitch, tjs_int w, tjs_int h)
{
    const tjs_uint8* p = pixel;
    tjs_uint32 clr = *(const tjs_uint32*)p;
    for (int y = 0; y < h; ++y)
    {
        const tjs_uint32* line = (const tjs_uint32*)p;
        for (int x = 0; x < w; ++x)
        {
            if (line[x] != clr)
            {
                return false;
            }
        }
        p += pitch;
    }
    return true;
}

class tTVPOGLTexture2D : public iTVPTexture2D
{
    friend class TVPRenderManager_OpenGL;

public:
    GLuint texture = 0;
    bool IsCompressed = false;

protected:
    TVPTextureFormat::e Format;
    unsigned int internalW;
    unsigned int internalH;
    unsigned char* PixelData = nullptr; // read only
    int PixelDataCounter = 0;
    float _scaleW = 1, _scaleH = 1;

    tTVPOGLTexture2D(unsigned int w,
                     unsigned int h,
                     TVPTextureFormat::e format,
                     GLint mode = GL_LINEAR)
      : iTVPTexture2D(w, h),
        Format(format)
    {
        if (mode)
        {
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }

    ~tTVPOGLTexture2D()
    {
        _totalVMemSize -= internalW * internalH * getPixelSize();
        if (PixelData)
            delete[] PixelData;
        if (texture)
            glDeleteTextures(1, (GLuint*)&texture);
    }

    int getPixelSize()
    {
        switch (Format)
        {
            case TVPTextureFormat::Gray:
                return 1;
            case TVPTextureFormat::RGB:
                return 3;
            case TVPTextureFormat::RGBA:
                return 4;
            default:
                return (int)Format;
        }
    }

    void InternalInit(const void* pixel, unsigned int intw, unsigned int inth, unsigned int pitch)
    {
        GLenum pixfmt = GL_RGBA;
        GLenum internalfmt = GL_RGBA;
        switch (Format)
        {
            case TVPTextureFormat::Gray:
                pixfmt = GL_LUMINANCE;
                internalfmt = GL_LUMINANCE;
                break;
            case TVPTextureFormat::RGB:
                pixfmt = GL_RGB;
                internalfmt = GL_RGB;
                break;
            case TVPTextureFormat::RGBA:
                pixfmt = GL_RGBA;
                internalfmt = GL_RGBA;
                break;
            default:
                break;
        }

        switch (pitch & 7)
        {
            case 0:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
                break;
            case 2:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                break;
            case 4:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                break;
            case 6:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                break;
            default:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                break;
        }

        _glBindTexture2D(texture);
        glTexImage2D(GL_TEXTURE_2D, 0, internalfmt, intw, inth, 0, pixfmt, GL_UNSIGNED_BYTE, pixel);

        internalW = intw;
        internalH = inth;
        _totalVMemSize += internalW * internalH * getPixelSize();
    }

    void InternalUpdate(const void* pixel, int pitch, int x, int y, int w, int h)
    {
        if (_scaleW != 1.f || _scaleH != 1.f)
        {
            RestoreNormalSize();
        }
        const unsigned char* src = (const unsigned char*)pixel;
        GLenum pixfmt;
        _glBindTexture2D(texture);
        unsigned int pixsize = Format & 0xF;
        switch (Format)
        {
            case TVPTextureFormat::Gray:
                pixfmt = GL_LUMINANCE;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                break;
            case TVPTextureFormat::RGB:
                pixfmt = GL_RGB;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                break;
            case TVPTextureFormat::RGBA:
                pixfmt = GL_RGBA;
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                break;
            default:
                return;
        }
        bool rearranged = false;
        if (GL_CHECK_unpack_subimage)
        {
            glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / pixsize);
        }
        else if (pitch != w * pixsize)
        {
            rearranged = true;
            unsigned int arrpitch = w * pixsize;
            unsigned char *arr = new unsigned char[arrpitch * h], *p = arr;
            for (int i = 0; i < h; ++i, p += arrpitch, src += pitch)
                memcpy(p, src, arrpitch);
            src = arr;
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, pixfmt, GL_UNSIGNED_BYTE, src);
        if (GL_CHECK_unpack_subimage)
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        if (rearranged)
            delete[] src;
    }

    bool RestoreNormalSize();

    virtual TVPTextureFormat::e GetFormat() const override { return Format; }

    virtual bool GetTextureData(void* picData, tjs_int& pic_pitch) override { return false; }

public:
    virtual bool IsOpaque() override
    {
        switch (Format)
        {
            case TVPTextureFormat::Gray:
            case TVPTextureFormat::RGB:
                return true;
            case TVPTextureFormat::RGBA:
            case TVPTextureFormat::None:
            case TVPTextureFormat::Compressed:
            case TVPTextureFormat::CompressedEnd:
                return false;
        }
        return false;
    }

    virtual bool GetScale(float& x, float& y) override
    {
        x = _scaleW;
        y = _scaleH;
        return true;
    }

    virtual const void* GetScanLineForRead(tjs_uint l) override;

    virtual tjs_uint32 GetPoint(int x, int y) override
    {
        if (PixelData)
            return *(uint32_t*)&PixelData[y * GetPitch() + x * 4];
        unsigned long clr = 0;
        TVPSetRenderTarget(texture);
        glViewport(0, 0, internalW, internalH);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x * _scaleW, y * _scaleH, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &clr);
        return clr;
    }

    virtual tjs_int GetPitch() const override
    {
        if (_scaleW == 1.f && _scaleH == 1.f)
            return internalW * 4;
        return tjs_int(internalW / _scaleW) * 4;
    }
    virtual tjs_uint GetInternalWidth() const override { return internalW; }
    virtual tjs_uint GetInternalHeight() const override { return internalH; }
    virtual void AsTarget() { assert(false); }
    virtual void SyncPixel()
    {
        if (PixelData && --PixelDataCounter <= 0)
        {
            delete[] PixelData;
            PixelData = nullptr;
        }
    }
    virtual void ApplyVertex(GLVertexInfo& vtx, const tTVPRect& rc)
    {
        vtx.tex = this;
        GLfloat sminu, smaxu, sminv, smaxv;
        sminu = (GLfloat)rc.left;
        smaxu = (GLfloat)rc.right;
        sminv = (GLfloat)rc.top;
        smaxv = (GLfloat)rc.bottom;
        float tw = _scaleW / internalW;
        float th = _scaleH / internalH;
        sminu *= tw;
        smaxu *= tw;
        sminv *= th;
        smaxv *= th;
        vtx.vtx.resize(6 * 2);
        GLfloat* pt = &vtx.vtx.front();
        pt[0] = sminu;
        pt[1] = sminv;
        pt[2] = smaxu;
        pt[3] = sminv;
        pt[4] = sminu;
        pt[5] = smaxv;

        pt[6] = smaxu;
        pt[7] = sminv;
        pt[8] = sminu;
        pt[9] = smaxv;
        pt[10] = smaxu;
        pt[11] = smaxv;
    }
    virtual void ApplyVertex(GLVertexInfo& vtx, const tTVPPointD* p, int n)
    {
        vtx.tex = this;
        vtx.vtx.resize(n * 2);
        GLfloat* pt = &vtx.vtx.front();
        float tw = _scaleW / internalW;
        float th = _scaleH / internalH;
        for (int i = 0; i < n; ++i)
        {
            pt[i * 2 + 0] = (float)p[i].x * tw;
            pt[i * 2 + 1] = (float)p[i].y * th;
        }
    }
    void Bind(unsigned int i) { _glBindTexture2DN(i, texture); }
};

static TVPTextureFormat::e _GetTextureFormatFromBPP(int bpp)
{
    switch (bpp)
    {
        case 8:
            return TVPTextureFormat::Gray;
        case 24:
            return TVPTextureFormat::RGB;
        case 32:
            return TVPTextureFormat::RGBA;
        default:
            return TVPTextureFormat::None;
    }
}

class tTVPOGLTexture2D_split : public tTVPOGLTexture2D
{
    struct GLTextureInfoPoint
    {
        uint16_t X, Y; // max to 65535 x 65535
    };

    union GLTextureInfoIndexer
    {
        uint32_t Index;
        GLTextureInfoPoint Point;
    };
    struct GLTextureInfo
    {
        GLuint Name = 0;
        //	uint32_t LastAccessTimeStamp;
        GLTextureInfoPoint Point;
        uint16_t Width = 0, Height = 0;
    };
    //	std::vector<GLuint> UnusedTextureName;
    std::map<uint32_t /*GLTextureInfoIndexer.Index*/, GLTextureInfo> CachedTexture;
    tTVPBitmap* Bitmap;

    void ClearTextureCache()
    {
        for (auto& it : CachedTexture)
        {
            glDeleteTextures(1, (GLuint*)&it.second.Name);
        }
        CachedTexture.clear();
        texture = 0;
    }

    GLuint FetchGLTexture()
    {
        GLuint ret;
        glGenTextures(1, &ret);
        glBindTexture(GL_TEXTURE_2D, ret);

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return ret;
    }

public:
    tTVPOGLTexture2D_split(tTVPBitmap* bmp)
      : tTVPOGLTexture2D(bmp->GetWidth(),
                         bmp->GetHeight(),
                         _GetTextureFormatFromBPP(bmp->GetBPP()),
                         0 /*don't generate texture*/)
    { // only accept from bitmap
        Bitmap = bmp;
        Bitmap->AddRef();
    }
    ~tTVPOGLTexture2D_split()
    {
        internalW = 0;
        internalH = 0;
        ClearTextureCache();
        Bitmap->Release();
    }
    virtual const void* GetScanLineForRead(tjs_uint l) override { return Bitmap->GetScanLine(l); }
    virtual tjs_int GetPitch() const override { return Bitmap->GetPitch(); }
    virtual tjs_uint32 GetPoint(int x, int y) override
    {
        if (Bitmap->Is32bit())
        {
            return ((uint32_t*)Bitmap->GetScanLine(y))[x];
        }
        else if (Bitmap->Is8bit())
        {
            return ((uint8_t*)Bitmap->GetScanLine(y))[x];
        }
        else
        {
            TVPThrowExceptionMessage(
                TJS_N("Unsupported texture format[RGB] for getting point color."));
            return 0;
        }
    }
    virtual void Update(const void* pixel,
                        TVPTextureFormat::e format,
                        int pitch,
                        const tTVPRect& rc) override
    {
        TVPThrowExceptionMessage(TJS_N("Static texture cannot update data."));
    }
    virtual void SetPoint(int x, int y, uint32_t clr) override
    {
        TVPThrowExceptionMessage(TJS_N("Static texture cannot set point color."));
    }
    virtual bool IsStatic() override { return true; }
    virtual bool IsOpaque() override
    {
        if (Bitmap)
            return Bitmap->IsOpaque;
        return false;
    }
    virtual void SyncPixel() override
    {
        // 		if (CachedTexture.size() > 1) {
        // 			;
        // 		}
    }

    void UpdateTextureData(GLTextureInfo& texinfo, const tTVPRect& rc)
    {
        unsigned int pixsize = Format & 0xF;
        GLenum pixfmt = GL_RGBA;
        GLenum internalfmt = GL_RGBA;
        switch (Format)
        {
            case TVPTextureFormat::Gray:
                pixfmt = GL_LUMINANCE;
                internalfmt = GL_LUMINANCE;
                break;
            case TVPTextureFormat::RGB:
                pixfmt = GL_RGB;
                internalfmt = GL_RGB;
                break;
            case TVPTextureFormat::RGBA:
                pixfmt = GL_RGBA;
                internalfmt = GL_RGBA;
                break;
            default:
                break;
        }

        unsigned int pitch = Bitmap->GetPitch();
        switch (pitch & 7)
        {
            case 0:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
                break;
            case 2:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                break;
            case 4:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                break;
            case 6:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                break;
            default:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                break;
        }

        _glBindTexture2D(texinfo.Name);
        texinfo.Width = pitch / pixsize;
        texinfo.Height = rc.get_height();
        if (texinfo.Width > GetMaxTextureWidth())
        {
            texinfo.Width = rc.get_width();
            assert(texinfo.Width <= GetMaxTextureWidth());
            glTexImage2D(GL_TEXTURE_2D, 0, internalfmt, texinfo.Width, texinfo.Height, 0, pixfmt,
                         GL_UNSIGNED_BYTE, nullptr);
            unsigned char* pix = nullptr;
            if (GL_CHECK_unpack_subimage)
            {
                pix = (unsigned char*)Bitmap->GetScanLine(rc.top);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, pitch / pixsize);
            }
            else
            {
                pix = new unsigned char[texinfo.Width * texinfo.Height * pixsize];
                unsigned char* src = (unsigned char*)Bitmap->GetScanLine(rc.top);
                unsigned char* dst = pix;
                int dpitch = texinfo.Width * pixsize;
                for (int y = 0; y < texinfo.Height; ++y)
                {
                    memcpy(dst, src, dpitch);
                    src += pitch;
                    dst += dpitch;
                }
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texinfo.Width, texinfo.Height, pixfmt,
                            GL_UNSIGNED_BYTE, pix);
            if (GL_CHECK_unpack_subimage)
            {
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
            else
            {
                delete[] pix;
            }
        }
        else
        {
            glTexImage2D(GL_TEXTURE_2D, 0, internalfmt, texinfo.Width, texinfo.Height, 0, pixfmt,
                         GL_UNSIGNED_BYTE, Bitmap->GetScanLine(rc.top));
        }
    }

    void AsSingleTexture()
    {
        ClearTextureCache();
        internalW = Bitmap->GetWidth(), internalH = Bitmap->GetHeight();
        if (internalW > GetMaxTextureWidth())
        {
            _scaleW = (float)GetMaxTextureWidth() / internalW;
            internalW = GetMaxTextureWidth();
        }
        if (internalH > GetMaxTextureHeight())
        {
            _scaleH = (float)GetMaxTextureHeight() / internalH;
            internalH = GetMaxTextureHeight();
        }

        int dpitch;
        unsigned char* tmp = TVPImageUtils::BilinearResizeRGBA(
            (const uint8_t*)Bitmap->GetBits(), Bitmap->GetWidth(), Bitmap->GetHeight(), Bitmap->GetPitch(),
            internalW, internalH, dpitch);

        Bitmap->Release(); // release here to relieve memory peak
        Bitmap = nullptr;

        unsigned int pixsize = Format & 0xF;
        GLenum pixfmt = GL_RGBA;
        GLenum internalfmt = GL_RGBA;
        switch (Format)
        {
            case TVPTextureFormat::Gray:
                pixfmt = GL_LUMINANCE;
                internalfmt = GL_LUMINANCE;
                break;
            case TVPTextureFormat::RGB:
                pixfmt = GL_RGB;
                internalfmt = GL_RGB;
                break;
            case TVPTextureFormat::RGBA:
                pixfmt = GL_RGBA;
                internalfmt = GL_RGBA;
                break;
            default:
                break;
        }

        unsigned int pitch = internalW * 4;
        switch (pitch & 7)
        {
            case 0:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
                break;
            case 4:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                break;
        }

        texture = FetchGLTexture();
        glTexImage2D(GL_TEXTURE_2D, 0, internalfmt, internalW, internalH, 0, pixfmt,
                     GL_UNSIGNED_BYTE, tmp);

        delete[] tmp;
    }

    virtual void ApplyVertex(GLVertexInfo& vtx, const tTVPPointD* p, int n) override
    {
        if (!Bitmap)
        { // route of downscaled single texture
            vtx.tex = this;
            vtx.vtx.resize(n * 2);
            GLfloat* pt = &vtx.vtx.front();
            float tw = _scaleW / internalW;
            float th = _scaleH / internalH;
            for (int i = 0; i < n; ++i)
            {
                pt[i * 2 + 0] = (float)p[i].x * tw;
                pt[i * 2 + 1] = (float)p[i].y * th;
            }
            return;
        }

        double l = Bitmap->GetWidth(), t = Bitmap->GetHeight(), r = 0, b = 0;
        for (int i = 0; i < n; ++i)
        {
            const tTVPPointD& pt = p[i];
            if (pt.x < l)
                l = pt.x;
            if (pt.y < t)
                t = pt.y;
            if (pt.x > r)
                r = pt.x;
            if (pt.y > b)
                b = pt.y;
        }
        tTVPRect rc(std::floor(l), std::floor(t), std::ceil(r), std::ceil(b));
        tjs_uint w = rc.get_width(), h = rc.get_height();
        if (w > GetMaxTextureWidth() || h > GetMaxTextureHeight())
        {
            AsSingleTexture();
            return ApplyVertex(vtx, p, n);
        }

        vtx.tex = this;
        GLTextureInfoIndexer indexer;
        indexer.Point.X = rc.left;
        indexer.Point.Y = rc.top;
        GLTextureInfo* texinfo;
        auto it = CachedTexture.find(indexer.Index);
        if (it != CachedTexture.end())
        {
            texinfo = &it->second;
            texture = texinfo->Name;
            w = std::min(w, Bitmap->GetWidth());
            h = std::min(h, Bitmap->GetHeight());
            if (texinfo->Width < w || texinfo->Height < h)
            {
                UpdateTextureData(*texinfo, rc);
            }
        }
        else
        {
            texture = FetchGLTexture();
            texinfo = &CachedTexture[indexer.Index];
            texinfo->Name = texture;
            texinfo->Point = indexer.Point;
            UpdateTextureData(*texinfo, rc);
        }

        vtx.vtx.resize(n * 2);
        GLfloat* pt = &vtx.vtx.front();
        for (int i = 0; i < n; ++i)
        {
            pt[i * 2 + 0] = p[i].x - rc.left;
            pt[i * 2 + 1] = p[i].y - rc.top;
        }
    }

    virtual void ApplyVertex(GLVertexInfo& vtx, const tTVPRect& rc) override
    {
        if (!Bitmap)
        {
            vtx.tex = this;
            GLfloat sminu, smaxu, sminv, smaxv;
            sminu = (GLfloat)rc.left;
            smaxu = (GLfloat)rc.right;
            sminv = (GLfloat)rc.top;
            smaxv = (GLfloat)rc.bottom;
            float tw = _scaleW / internalW;
            float th = _scaleH / internalH;
            sminu *= tw;
            smaxu *= tw;
            sminv *= th;
            smaxv *= th;
            vtx.vtx.resize(6 * 2);
            GLfloat* pt = &vtx.vtx.front();
            pt[0] = sminu;
            pt[1] = sminv;
            pt[2] = smaxu;
            pt[3] = sminv;
            pt[4] = sminu;
            pt[5] = smaxv;

            pt[6] = smaxu;
            pt[7] = sminv;
            pt[8] = sminu;
            pt[9] = smaxv;
            pt[10] = smaxu;
            pt[11] = smaxv;
            return;
        }
        tjs_uint w = rc.get_width(), h = rc.get_height();
        if (w > GetMaxTextureWidth() || h > GetMaxTextureHeight())
        {
            AsSingleTexture();
            return ApplyVertex(vtx, rc);
        }
        vtx.tex = this;
        GLTextureInfoIndexer indexer;
        indexer.Point.X = rc.left;
        indexer.Point.Y = rc.top;
        GLTextureInfo* texinfo;
        auto it = CachedTexture.find(indexer.Index);
        if (it != CachedTexture.end())
        {
            texinfo = &it->second;
            texture = texinfo->Name;
            w = std::min(w, Bitmap->GetWidth());
            h = std::min(h, Bitmap->GetHeight());
            if (texinfo->Width < w || texinfo->Height < h)
            {
                UpdateTextureData(*texinfo, rc);
            }
            //	it->second.LastAccessTimeStamp = 120; // max to 120 frames ?
        }
        else
        {
            texture = FetchGLTexture();
            texinfo = &CachedTexture[indexer.Index];
            texinfo->Name = texture;
            //	texinfo->LastAccessTimeStamp = 120;
            texinfo->Point = indexer.Point;
            UpdateTextureData(*texinfo, rc);
        }

        vtx.vtx.resize(6 * 2);
        GLfloat* pt = &vtx.vtx.front();
        GLfloat sminu, smaxu, sminv, smaxv;
        sminu = (GLfloat)0;
        smaxu = (GLfloat)w / texinfo->Width;
        sminv = (GLfloat)0;
        smaxv = (GLfloat)h / texinfo->Height;

        pt[0] = sminu;
        pt[1] = sminv;
        pt[2] = smaxu;
        pt[3] = sminv;
        pt[4] = sminu;
        pt[5] = smaxv;

        pt[6] = smaxu;
        pt[7] = sminv;
        pt[8] = sminu;
        pt[9] = smaxv;
        pt[10] = smaxu;
        pt[11] = smaxv;
    }
};

class tTVPOGLTexture2D_static : public tTVPOGLTexture2D
{
public:
    // for manual init
    tTVPOGLTexture2D_static(TVPTextureFormat::e format,
                            unsigned int tw,
                            unsigned int th,
                            float sw,
                            float sh,
                            GLint mode = GL_LINEAR)
      : tTVPOGLTexture2D(tw, th, format, mode)
    {
        _scaleW = sw;
        _scaleH = sh;
    }

    tTVPOGLTexture2D_static(const void* pixel,
                            int pitch,
                            unsigned int iw,
                            unsigned int ih,
                            TVPTextureFormat::e format,
                            unsigned int tw,
                            unsigned int th,
                            float sw,
                            float sh,
                            GLint mode = GL_LINEAR)
      : tTVPOGLTexture2D(tw, th, format, mode)
    {
        _scaleW = sw;
        _scaleH = sh;
        //	assert(pixel); // pixel must be exist
        int pixsize = getPixelSize();
        if (pitch == iw * pixsize || ((pitch & 7) == 0 && pitch - iw * pixsize < 8))
        {
            InternalInit(pixel, iw, ih, pitch);
        }
        else if (GL_CHECK_unpack_subimage)
        {
            InternalInit(nullptr, iw, ih, 0);
            InternalUpdate(pixel, pitch, 0, 0, iw, ih);
        }
        else
        { // rearrange
            InternalInit(nullptr, iw, ih, 0);
            PixelData = new unsigned char[internalW * internalH * 4];
            int linesize = internalW * pixsize, dstpitch = internalW * 4;
            unsigned char* src = (unsigned char*)pixel;
            unsigned char* dst = (unsigned char*)PixelData;
            for (unsigned int y = 0; y < internalH; ++y)
            {
                memcpy(dst, src, linesize);
                src += pitch;
                dst += dstpitch;
            }
            InternalUpdate(PixelData, dstpitch, 0, 0, internalW, internalH);
            delete[] PixelData;
            PixelData = nullptr;
        }
        //_totalVMemSize += internalW * internalH * pixsize;
    }

    // for compressed texture format
    tTVPOGLTexture2D_static(const void* data,
                            int len,
                            GLenum format,
                            unsigned int tw,
                            unsigned int th,
                            unsigned int iw,
                            unsigned int ih,
                            float sw,
                            float sh)
      : tTVPOGLTexture2D(tw, th, TVPTextureFormat::RGBA, GL_LINEAR)
    {
        _scaleW = sw;
        _scaleH = sh;
        InitCompressedPixel(data, len, format, iw, ih);
    }

    void InitCompressedPixel(
        const void* data, unsigned int len, GLenum format, unsigned int width, unsigned int height)
    {
        _glBindTexture2D(texture);
        glCompressedTexImage2D(GL_TEXTURE_2D, 0, format /*GL_ETC1_RGB8_OES*/, width, height, 0, len,
                               data);
        IsCompressed = true;
        internalW = width;
        internalH = height;
        _totalVMemSize += internalW * internalH * getPixelSize() /*len*/;
    }

    void InitPixel(const void* pixel,
                   unsigned int pitch,
                   GLenum format,
                   GLenum pixfmt,
                   unsigned int intw,
                   unsigned int inth)
    {
        _glBindTexture2D(texture);
        glTexImage2D(GL_TEXTURE_2D, 0, format, intw, inth, 0, pixfmt, GL_UNSIGNED_BYTE, pixel);

        switch (pitch & 7)
        {
            case 0:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
                break;
            case 2:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                break;
            case 4:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                break;
            case 6:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
                break;
            default:
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                break;
        }

        internalW = intw;
        internalH = inth;
        _totalVMemSize += internalW * internalH * getPixelSize();
    }

    virtual void Update(const void* pixel,
                        TVPTextureFormat::e format,
                        int pitch,
                        const tTVPRect& rc) override
    {
        if (PixelData)
        {
            delete[] PixelData;
            PixelData = nullptr;
        }
        InternalUpdate(pixel, pitch, rc.left, rc.top, rc.get_width(), rc.get_height());
    };

    virtual void* GetScanLineForWrite(tjs_uint l) override
    {
        assert(false);
        return nullptr;
    }

    virtual void SetPoint(int x, int y, uint32_t clr) override
    {
        if (texture)
        {
            _glBindTexture2D(texture);
            // glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &clr);
        }
    }

    virtual void SetSize(unsigned int w, unsigned int h) override { assert(false); }

    virtual bool IsStatic() override { return true; }
};

class tTVPOGLTexture2D_mutatble : public tTVPOGLTexture2D
{
    bool IsTextureDirty;

public:
    tTVPOGLTexture2D_mutatble(const void* pixel,
                              int pitch,
                              unsigned int w,
                              unsigned int h,
                              TVPTextureFormat::e format,
                              float sw,
                              float sh)
      : tTVPOGLTexture2D(
            w, h, format == TVPTextureFormat::RGB ? TVPTextureFormat::RGBA : format, GL_LINEAR)
    {
        if (!pixel)
        {
            _scaleW = sw;
            _scaleH = sh;
            unsigned int intw = power_of_two(w * sw), inth = power_of_two(h * sh);
            if (intw < w)
                _scaleW = (float)intw / w; // use entire texture if possible
            if (inth < h)
                _scaleH = (float)inth / h;
            InternalInit(nullptr, intw, inth, 0);
            IsTextureDirty = false;
            return;
        }
        assert(sw == 1.f && sh == 1.f);

        int pixsize = getPixelSize();
        int intw = w, inth = h;
        if (pixel)
            intw = pitch / pixsize;
        intw = power_of_two(intw), inth = power_of_two(inth);
        if (!pixel || pitch == intw * pixsize)
        {
            if (inth == h)
            {
                InternalInit(pixel, intw, inth, pitch);
                IsTextureDirty = false;
            }
            else
            {
                InternalInit(nullptr, intw, inth, 0);
                IsTextureDirty = false;
                if (pixel)
                    Update(pixel, Format, pitch, tTVPRect(0, 0, w, h));
            }
        }
        else if (w == Width)
        {
            InternalInit(nullptr, intw, inth, 0);
            Update(pixel, Format, pitch, tTVPRect(0, 0, pitch / pixsize, h));
        }
        else if (GL_CHECK_unpack_subimage || pitch == w * pixsize)
        {
            InternalInit(nullptr, intw, inth, 0);
            Update(pixel, Format, pitch, tTVPRect(0, 0, w, h));
        }
        else
        {
            InternalInit(nullptr, intw, inth, 0);
            PixelData = new unsigned char[internalW * internalH * 4];
            int linesize = w * pixsize, dstpitch = internalW * 4;
            unsigned char* src = (unsigned char*)pixel;
            unsigned char* dst = (unsigned char*)PixelData;
            for (unsigned int y = 0; y < h; ++y)
            {
                memcpy(dst, src, linesize);
                src += pitch;
                dst += dstpitch;
            }
            IsTextureDirty = true;
        }
    }

    virtual void SyncPixel() override
    {
        if (PixelData)
        {
            if (IsTextureDirty)
            {
                InternalUpdate(PixelData, internalW * 4, 0, 0, internalW, internalH);
                IsTextureDirty = false;
                delete[] PixelData;
                PixelData = nullptr;
            }
            else if (--PixelDataCounter <= 0)
            {
                IsTextureDirty = false;
                delete[] PixelData;
                PixelData = nullptr;
            }
        }
    }

    virtual void Update(const void* pixel,
                        TVPTextureFormat::e format,
                        int pitch,
                        const tTVPRect& rc) override
    {
        if (PixelData)
        {
            if (rc.left > 0 || rc.top > 0 || rc.bottom < Height || rc.right < Width)
            {
                unsigned char *src = (unsigned char*)pixel, *dst = (unsigned char*)PixelData;
                int dpitch = internalW * 4;
                for (int y = 0; y < Height; ++y)
                {
                    memcpy(dst, src, dpitch);
                    src += pitch;
                    dst += dpitch;
                }
                pixel = PixelData;
                pitch = internalW * 4;
                PixelDataCounter = 5;
            }
            else
            {
                delete[] PixelData;
                PixelData = nullptr;
            }
            IsTextureDirty = false;
        }
        InternalUpdate(pixel, pitch, rc.left, rc.top, rc.get_width(), rc.get_height());
    }

    virtual void* GetScanLineForWrite(tjs_uint l) override
    {
        IsTextureDirty = true;
        return (void*)GetScanLineForRead(l);
    }

    virtual void SetPoint(int x, int y, tjs_uint32 clr) override
    {
        if (texture)
        {
            SyncPixel();
            _glBindTexture2D(texture);
            // glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &clr);
        }
    }

    virtual void SetSize(unsigned int w, unsigned int h) override
    {
        if (w > internalW || h > internalH)
        {
            if (PixelData)
            {
                delete[] PixelData;
                PixelData = nullptr;
            }
            InternalInit(nullptr, power_of_two(w), power_of_two(h), 0);
        }
        Width = w;
        Height = h;
    }

    virtual bool IsStatic() override { return false; }

    virtual void AsTarget() override
    {
        SyncPixel();
        TVPSetRenderTarget(texture);
    }
};

class tTVPOGLRenderMethod : public iTVPRenderMethod
{
protected:
    friend class TVPRenderManager_OpenGL;
    GLint pos_attr_location;
    std::vector<GLint> tex_coord_attr_location;
    GLenum program;
    int BlendFunc = 0, BlendSrcRGB, BlendDstRGB, BlendSrcA, BlendDstA;
    // 	typedef bool(tTVPOGLRenderMethod::*FCustomProc)(tTVPOGLTexture2D *, const tTVPRect&, const
    // std::vector<GLVertexInfo>&); 	FCustomProc CustomProc;
    bool tar_as_src;

public:
    tTVPOGLRenderMethod() : tar_as_src(false) /*, CustomProc(nullptr)*/ {}
    GLint GetPosAttr() const { return pos_attr_location; }
    GLint GetTexCoordAttr(unsigned int n)
    {
        if (n >= tex_coord_attr_location.size())
            return -1;
        return tex_coord_attr_location[n];
    }
    virtual void Rebuild() = 0;
    virtual void Apply() = 0;
    virtual void onFinish() {}
    tTVPOGLRenderMethod* SetTargetAsSrc()
    {
        tar_as_src = true;
        return this;
    }
    virtual tTVPOGLRenderMethod* SetBlendFuncSeparate(
        int func, int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) override
    {
        BlendFunc = func;
        BlendSrcRGB = srcRGB;
        BlendDstRGB = dstRGB;
        BlendSrcA = srcAlpha;
        BlendDstA = dstAlpha;
        return this;
    }
    virtual bool IsBlendTarget() override { return !!BlendFunc; }
    virtual void ApplyTexture(unsigned int i, const GLVertexInfo& info)
    {
        info.tex->Bind(i);
        glVertexAttribPointer(GetTexCoordAttr(i), 2, GL_FLOAT, GL_FALSE, 0, &info.vtx.front());
    }

    bool (*CustomProc)(tTVPOGLRenderMethod* _method,
                       tTVPOGLTexture2D* _tar,
                       tTVPOGLTexture2D* reftar,
                       const tTVPRect* rctar,
                       const tRenderTexRectArray& textures) = nullptr;
};

static GLenum CompileShader(GLint shadertype, const std::string& src)
{
    GLenum shader = glCreateShader(shadertype);
    GLint status;
    const char* pszSource = src.c_str();

    glShaderSource(shader, 1, &pszSource, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == 0)
    {
        GLint length;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
        char* info = new char[length + 1];
        info[length] = 0;
        glGetShaderInfoLog(shader, length, NULL, info);
        ttstr _info(info);
        delete[] info;
        TVPThrowExceptionMessage(TJS_N("Failed to compile shader: %1"), _info);
        return 0;
    }
    else
    {
        return shader;
    }
}

static GLenum CombineProgram(GLenum vert, GLenum frag)
{
    /* Create one program object to rule them all */
    GLenum program = glCreateProgram();
    /* ... and in the darkness bind them */
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    GLint linkSuccessful;
    glGetProgramiv(program, GL_LINK_STATUS, &linkSuccessful);
    if (!linkSuccessful)
    {
        GLint length = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
        char* info = new char[length + 1];
        info[length] = 0;
        glGetProgramInfoLog(program, length, NULL, info);
        TVPThrowExceptionMessage(TJS_N("Failed to link shader program:\n%1"), info);
        delete[] info;
        return -1;
    }

    return program;
}

class tTVPOGLRenderMethod_Script : public tTVPOGLRenderMethod
{
protected:
    std::string m_strScript;
    int m_nTex;

public:
    int GetValidTex() { return m_nTex - tar_as_src; }
    bool init(const std::string& body, int nTex, const char* prefix = nullptr)
    {
        m_nTex = nTex;
        std::ostringstream str;
        if (prefix)
            str << prefix;
        str << "#ifdef GL_ES\n"
               "precision mediump float;\n"
               "#endif\n";
        std::ostringstream unif;
        for (int i = 0; i < nTex; ++i)
        {
            str << "varying vec2 v_texCoord";
            str << i;
            str << ";\n";

            unif << "uniform sampler2D tex";
            unif << i;
            unif << ";\n";
        }
        str << unif.str();
        str << body;
        m_strScript = str.str();
        Rebuild();
        return true;
    }
    virtual void SetParameterColor4B(int id, unsigned int clr)
    {
        glUseProgram(program);
        glUniform4f(id, (clr & 0xFF) / 255.0f, ((clr >> 8) & 0xFF) / 255.0f,
                    ((clr >> 16) & 0xFF) / 255.0f, (clr >> 24) / 255.0f);
    };
    virtual int EnumParameterID(const char* name)
    {
        int ret = glGetUniformLocation(program, name);
        return ret;
    }
    virtual void SetParameterOpa(int id, int Value)
    {
        glUseProgram(program);
        glUniform1f(id, Value / 255.f);
    };
    virtual void SetParameterFloat(int id, float Value)
    {
        glUseProgram(program);
        glUniform1f(id, Value);
    }
    virtual void SetParameterFloatArray(int id, float* Value, int nElem)
    {
        glUseProgram(program);
        switch (nElem)
        {
            case 2:
                glUniform2f(id, Value[0], Value[1]);
                break;
            case 3:
                glUniform3f(id, Value[0], Value[1], Value[2]);
                break;
            case 4:
                glUniform4f(id, Value[0], Value[1], Value[2], Value[4]);
                break;
        }
    }

    static std::vector<GLenum> m_cachedSahder;
    static void ClearCache() { m_cachedSahder.clear(); }
    static GLenum GetVertShader(unsigned int nTex)
    {
        if (nTex >= m_cachedSahder.size())
        {
            m_cachedSahder.resize(nTex + 1);
        }
        if (!m_cachedSahder[nTex])
        {
            std::ostringstream shader;
            shader << "#ifdef GL_ES\n"
                      "precision mediump float;\n"
                      "#endif\n";
            shader << "attribute vec2 a_position;\n";
            std::ostringstream body;
            body << "void main() {\n"
                    "    gl_Position = vec4(a_position, 0.0, 1.0);\n";
            std::ostringstream vary;
            for (unsigned int i = 0; i < nTex; ++i)
            {
                shader << "attribute vec2 a_texCoord";
                shader << i;
                shader << ";\n"; // attribute vec2 a_texCoord0;
                vary << "varying vec2 v_texCoord";
                vary << i;
                vary << ";\n"; // varying vec2 v_texCoord0;
                body << "    v_texCoord";
                body << i;
                body << " = a_texCoord";
                body << i;
                body << ";\n"; // v_texCoord0 = a_texCoord0;
            }
            shader << vary.str();
            shader << body.str();
            shader << "}";
            m_cachedSahder[nTex] = CompileShader(GL_VERTEX_SHADER, shader.str());
        }
        return m_cachedSahder[nTex];
    }

    virtual void Rebuild()
    {
        try
        {
            program = CombineProgram(GetVertShader(m_nTex),
                                     CompileShader(GL_FRAGMENT_SHADER, m_strScript));
        }
        catch (eTJSError& e)
        {
            e.AppendMessage("\n");
            e.AppendMessage(Name);
            throw;
        }
        glUseProgram(program);
        std::string tex("tex");
        std::string coord("a_texCoord");
        for (int i = 0; i < m_nTex; ++i)
        {
            char sCounter[8];
            sprintf(sCounter, "%d", i);
            int loc = glGetUniformLocation(program, (tex + sCounter).c_str());
            glUniform1i(loc, i);
            loc = glGetAttribLocation(program, (coord + sCounter).c_str());
            tex_coord_attr_location.push_back(loc);
        }
        pos_attr_location = glGetAttribLocation(program, "a_position");
    }
    virtual void Apply()
    {
        glUseProgram(program);
        if (BlendFunc)
        {
            glEnable(GL_BLEND);
            glBlendEquation(BlendFunc);
            glBlendFuncSeparate(BlendSrcRGB, BlendDstRGB, BlendSrcA, BlendDstA);
        }
        else
        {
            glDisable(GL_BLEND);
        }
    }
    const std::string& GetScript() { return m_strScript; }
};
std::vector<GLenum> tTVPOGLRenderMethod_Script::m_cachedSahder;

#if _KRKRSDL3_GL
class tTVPOGLRenderMethod_FillARGB : public tTVPOGLRenderMethod_Script
{
    typedef tTVPOGLRenderMethod_Script inherit;
    uint32_t m_color;

public:
    tTVPOGLRenderMethod_FillARGB()
    {
        if (glClearTexImage && glClearTexSubImage)
        {
            CustomProc = &tTVPOGLRenderMethod_FillARGB::DoFill;
        }
    }

    virtual int EnumParameterID(const char* name) override { return 0; }
    virtual void SetParameterColor4B(int id, unsigned int clr) override
    {
        if (CustomProc)
            m_color = clr;
        else
            inherit::SetParameterColor4B(id, clr);
    };

    static bool DoFill(tTVPOGLRenderMethod* method,
                       tTVPOGLTexture2D* tar,
                       tTVPOGLTexture2D*,
                       const tTVPRect* rctar,
                       const tRenderTexRectArray&)
    {
        uint32_t c = static_cast<tTVPOGLRenderMethod_FillARGB*>(method)->m_color;
        uint8_t clr[4] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), (uint8_t)(c >> 24)};
        if (rctar->left <= 0 && rctar->top <= 0 && rctar->get_width() >= tar->GetWidth() &&
            rctar->get_height() >= tar->GetHeight())
        {
            glClearTexImage(tar->texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, clr);
        }
        else
        {
            float sw, sh;
            tar->GetScale(sw, sh);
            if (sw != 1.f && sh != 1.f)
                return false;
            glClearTexSubImage(tar->texture, 0, rctar->left, rctar->top, 0, rctar->get_width(),
                                   rctar->get_height(), 1, GL_RGBA, GL_UNSIGNED_BYTE, clr);
        }
        return true;
    }
};

class tTVPOGLRenderMethod_AlphaTest : public tTVPOGLRenderMethod_Script
{
    typedef tTVPOGLRenderMethod_Script inherit;

public:
    virtual int EnumParameterID(const char* name) override
    {
        if (!strcmp("alpha_threshold", name))
        {
            return 0xA19A1E21;
        }
        else
        {
            return inherit::EnumParameterID(name);
        }
    }

    virtual void SetParameterOpa(int id, int Value) override
    {
        if (id == 0xA19A1E21)
        {
            glAlphaFunc(GL_GREATER, Value / 255.f);
        }
        else
        {
            return inherit::SetParameterOpa(id, Value);
        }
    }

    virtual void Apply() override
    {
        inherit::Apply();
        glEnable(GL_ALPHA_TEST);
    }

    virtual void onFinish() override { glDisable(GL_ALPHA_TEST); }
};
#endif

class tTVPOGLRenderMethod_Script_BlendColor : public tTVPOGLRenderMethod_Script
{
    typedef tTVPOGLRenderMethod_Script inherit;
    virtual int EnumParameterID(const char* name) override
    {
        if (!strcmp(name, "opacity"))
        {
            return 0x709AC167;
        }
        return inherit::EnumParameterID(name);
    }
    virtual void SetParameterOpa(int id, int Value) override
    {
        if (id == 0x709AC167)
        {
            float v = Value / 255.f;
            glUseProgram(program);
            glBlendColor(v, v, v, v);
        }
        else
        {
            inherit::SetParameterOpa(id, Value);
        }
    };
    virtual void SetParameterFloat(int id, float Value) override
    {
        if (id == 0x709AC167)
        {
            float v = Value;
            glUseProgram(program);
            glBlendColor(v, v, v, v);
        }
        else
        {
            inherit::SetParameterFloat(id, Value);
        }
    };
};

class tTVPOGLRenderMethod_AdjustGamma : public tTVPOGLRenderMethod_Script
{
    typedef tTVPOGLRenderMethod_Script inherit;
    virtual int EnumParameterID(const char* name)
    {
        if (!strcmp(name, "gammaAdjustData"))
        {
            return 0x3a33aad6;
        }
        return inherit::EnumParameterID(name);
    }
    virtual void SetParameterPtr(int id, const void* v)
    {
        if (id == 0x3a33aad6)
        {
            const tTVPGLGammaAdjustData& data = *(const tTVPGLGammaAdjustData*)v;
            static int id_gamma = EnumParameterID("u_gamma"), id_floor = EnumParameterID("u_floor"),
                       id_amp = EnumParameterID("u_amp");
            glUseProgram(program);
            glUniform3f(id_gamma, 1.0f / data.RGamma, 1.0f / data.GGamma, 1.0f / data.BGamma);
            glUniform3f(id_floor, data.RFloor / 255.0f, data.GFloor / 255.0f, data.BFloor / 255.0f);
            glUniform3f(id_amp, (data.RCeil - data.RFloor) / 255.0f,
                        (data.GCeil - data.GFloor) / 255.0f, (data.BCeil - data.BFloor) / 255.0f);
        }
        else
        {
            inherit::SetParameterPtr(id, v);
        }
    }
};

class tTVPOGLRenderMethod_UnivTrans : public tTVPOGLRenderMethod_Script
{
    typedef tTVPOGLRenderMethod_Script inherit;
    int u_phase, u_vague;
    virtual int EnumParameterID(const char* name)
    {
        int ret = inherit::EnumParameterID(name);
        if (!strcmp(name, "phase"))
        {
            u_phase = ret;
        }
        else if (!strcmp(name, "vague"))
        {
            u_vague = ret;
        }
        return ret;
    }
    int m_vague;
    virtual void SetParameterInt(int id, int Value)
    {
        glUseProgram(program);
        if (id == u_vague)
        {
            m_vague = Value;
            glUniform1f(id, Value / 255.f);
        }
        else if (id == u_phase)
        {
            Value -= m_vague;
            glUniform1f(id, Value / 255.f);
        }
        else
        {
            return inherit::SetParameterInt(id, Value);
        }
    }
};

class tTVPOGLRenderMethod_BoxBlur : public tTVPOGLRenderMethod_Script
{
    typedef tTVPOGLRenderMethod_Script inherit;
    enum eArea
    {
        eAreaLeft = 1,
        eAreaTop,
        eAreaRight,
        eAreaBottom,
    };
    tTVPRect area;
    virtual int EnumParameterID(const char* name)
    {
        if (!strcmp(name, "area_left"))
        {
            return eAreaLeft;
        }
        else if (!strcmp(name, "area_top"))
        {
            return eAreaTop;
        }
        else if (!strcmp(name, "area_right"))
        {
            return eAreaRight;
        }
        else if (!strcmp(name, "area_bottom"))
        {
            return eAreaBottom;
        }
        return inherit::EnumParameterID(name);
        ;
    }

    virtual void SetParameterInt(int id, int Value)
    {
        switch (id)
        {
            case eAreaLeft:
                area.left = Value;
                break;
            case eAreaTop:
                area.top = Value;
                break;
            case eAreaRight:
                area.right = Value;
                break;
            case eAreaBottom:
                area.bottom = Value;
                break;
        }
    }

    virtual void ApplyTexture(unsigned int i, const GLVertexInfo& info)
    {
        if (i == 0)
        {
            static GLint u_pos = glGetUniformLocation(program, "u_sample");
            float w, h;
            info.tex[0].GetScale(w, h);
            w *= info.tex[0].GetInternalWidth();
            h *= info.tex[0].GetInternalHeight();
            float pos[8 * 2];
            float l = area.left / w, t = area.top / h, r = area.right / w, b = area.bottom / h;
            pos[0] = l;
            pos[1] = t;
            pos[2] = 0;
            pos[3] = t;
            pos[4] = r;
            pos[5] = t;
            pos[6] = l;
            pos[7] = 0;
            pos[8] = r;
            pos[9] = 0;
            pos[10] = l;
            pos[11] = b;
            pos[12] = 0;
            pos[13] = b;
            pos[14] = r;
            pos[15] = b;
            glUniform2fv(u_pos, 8, pos);
        }
        inherit::ApplyTexture(i, info);
    }
};

bool tTVPOGLTexture2D::RestoreNormalSize()
{
    tjs_uint w = internalW / _scaleW, h = internalH / _scaleH;
    if (w < GetMaxTextureWidth() && h < GetMaxTextureHeight())
    {
        GLuint newtex;
        glGenTextures(1, &newtex);
        glBindTexture(GL_TEXTURE_2D, newtex);
        tjs_int intw = power_of_two(w), inth = power_of_two(h);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, intw, inth, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLfloat minx = -1, maxx = 1, miny = -1, maxy = 1;
        static const GLfloat vertices[12] = {minx, miny, maxx, miny, minx, maxy,

                                             maxx, miny, minx, maxy, maxx, maxy};
        static tTVPOGLRenderMethod* method =
            (tTVPOGLRenderMethod*)TVPGetRenderManager()->GetRenderMethod("Copy");
        method->Apply();
        TVPSetRenderTarget(newtex);
        glViewport(0, 0, w, h);
        int VA_flag = 1 << method->GetPosAttr();
        for (unsigned int i = 0; i < 1; ++i)
        {
            VA_flag |= 1 << method->GetTexCoordAttr(i);
        }
        _glEnableVertexAttribs(VA_flag);
        glVertexAttribPointer(method->GetPosAttr(), 2, GL_FLOAT, GL_FALSE, 0, vertices);

        GLVertexInfo vtx;
        ApplyVertex(vtx, tTVPRect(0, 0, w, h));
        glBindTexture(GL_TEXTURE_2D, texture);
        glVertexAttribPointer(method->GetTexCoordAttr(0), 2, GL_FLOAT, GL_FALSE, 0,
                              &vtx.vtx.front());
        glDrawArrays(GL_TRIANGLES, 0, 6);

        _totalVMemSize -= internalW * internalH * getPixelSize();
        glDeleteTextures(1, (GLuint*)&texture);

        texture = newtex;
        internalW = intw;
        internalH = inth;
        _totalVMemSize += internalW * internalH * getPixelSize();
        _scaleW = _scaleH = 1;
        return true;
    }
    else
    {
        TVPShowSimpleMessageBox(ttstr(TJS_N("Too large texture is not accessable by GPU.")),
                                TJS_N("Please use software renderer"));
        return false;
    }
}

const void* tTVPOGLTexture2D::GetScanLineForRead(tjs_uint l)
{
    PixelDataCounter = 5;
    if (_scaleW == 1.f && _scaleH == 1.f)
    {
        if (!PixelData)
        {
            PixelData = new unsigned char[internalW * internalH * 4];
            TVPSetRenderTarget(texture);
            glViewport(0, 0, internalW, internalH);
            glPixelStorei(GL_PACK_ALIGNMENT, 4); // always dword aligned
            glReadPixels(0, 0, internalW, internalH, GL_RGBA, GL_UNSIGNED_BYTE, PixelData);
        }
        return &PixelData[l * internalW * 4];
    }
    else
    {
        if (!PixelData)
        {
            if (RestoreNormalSize())
                return GetScanLineForRead(l); // route of 1:1 size
            return nullptr;
        }
        tjs_int w = internalW / _scaleW;
        return &PixelData[l * w * 4];
    }
}

#ifdef TEST_SHADER_ENABLED
#include "RenderManager_ogl_test.hpp"
#else
#define TEST_SHADER(...)
#define TEST_SHADER_IGNORE_ALPHA(...)
#endif

static iTVPTexture2D* (*_CreateStaticTexture2D)(const void* dib,
                                                tjs_uint tw,
                                                tjs_uint th,
                                                tjs_int pitch,
                                                TVPTextureFormat::e fmt,
                                                bool isOpaque);
static iTVPTexture2D* (*_CreateMutableTexture2D)(
    const void* pixel, int pitch, unsigned int w, unsigned int h, TVPTextureFormat::e format);
static const char* _glExtensions = nullptr;
// static bool _duplicateTargetTexture = true;
class TVPRenderManager_OpenGL : public iTVPRenderManager
{
protected:
    virtual tTVPOGLRenderMethod_Script* GetRenderMethodFromScript(const char* script,
                                                                  int nTex,
                                                                  unsigned int flags) override
    {
        tTVPOGLRenderMethod_Script* method = new tTVPOGLRenderMethod_Script();
        method->init(script, nTex);
        if (flags & RENDER_METHOD_FLAG_TARGET_AS_INPUT)
        {
            method->SetTargetAsSrc();
        }
        return method;
    }

    template<typename T>
    T* CompileAndRegScript(const char* name,
                           const std::string& script,
                           int nTex,
                           const char* prefix = nullptr)
    {
        T* method = new T();
        method->init(script, nTex, prefix);
        RegisterRenderMethod(name, method);
        return method;
    }

    const char* const common_shader_framebuffer_fetch_prefix =
        "#if defined(GL_ARM_shader_framebuffer_fetch)"
        "\n"
        "#extension GL_ARM_shader_framebuffer_fetch : require"
        "\n"
        "#define gl_LastFragColor gl_LastFragColorARM"
        "\n"
        "#elif defined(GL_EXT_shader_framebuffer_fetch)"
        "\n"
        "#extension GL_EXT_shader_framebuffer_fetch : require"
        "\n"
        "#define gl_LastFragColor gl_LastFragData[0]"
        "\n"
        "#elif defined(GL_NV_shader_framebuffer_fetch)"
        "\n"
        "#extension GL_NV_shader_framebuffer_fetch : require"
        "\n"
        "#else"
        "\n"
        "#error noy any framebuffer fetch extension available"
        "\n"
        "#endif"
        "\n";

    const std::string ScriptCommonPrefix = "void main() {\n"
                                           "    vec4 s = texture2D(tex0, v_texCoord0);\n";
    const std::string ScriptCommonBltPrefix =
        ScriptCommonPrefix + "    vec4 d = texture2D(tex1, v_texCoord1);\n";
    const std::string ScriptCommonFBFBltPrefix =
        ScriptCommonPrefix + "    vec4 d = gl_LastFragColor;\n";

    template<typename T = tTVPOGLRenderMethod_Script>
    T* CompileAndRegRegularBlendMethod(const char* name,
                                       const std::string& prefix,
                                       const std::string& code)
    {
        T* method = CompileAndRegScript<T>(name, prefix + ScriptCommonBltPrefix + code, 2);
        method->SetTargetAsSrc();
        return method;
    }

    static iTVPTexture2D* CreateStaticTexture2D(const void* dib,
                                                tjs_uint tw,
                                                tjs_uint th,
                                                tjs_int pitch,
                                                TVPTextureFormat::e fmt,
                                                tjs_uint dstw,
                                                tjs_uint dsth,
                                                bool isOpaque)
    {
        const tjs_uint8* pixel = (const tjs_uint8*)dib;
        tjs_uint8* tmp = nullptr;
        tjs_uint w = tw, h = th;
        float sw = 1.f, sh = 1.f;
        if (dstw != tw || dsth != th)
        {
            tjs_uint dpitch;
            tmp = (fmt == TVPTextureFormat::RGBA ? TVPShrink : TVPShrink_8)(&dpitch, pixel, pitch,
                                                                            tw, th, dstw, dsth);
            pixel = tmp;
            h = dsth;
            w = dstw;
            pitch = dpitch;
            sw = (float)(dstw - (tw & 1)) / tw;
            sh = (float)(dsth - (th & 1)) / th;
            if (!isOpaque)
            {
                isOpaque = TVPCheckOpaqueRGBA(pixel, dpitch, w, h);
            }
        }
        if (fmt == TVPTextureFormat::RGBA && isOpaque)
        {
            int dpitch = (w * 3 + 7) & ~7;
            tjs_uint8* rgb = new tjs_uint8[dpitch * h + 16];
            tjs_uint8* dst = (tjs_uint8*)(((intptr_t)rgb + 7) & ~7);
            const tjs_uint8* src = (const tjs_uint8*)pixel;
            pixel = dst;
            for (int y = 0; y < h; ++y)
            {
                TVPConvert32BitTo24Bit(dst, src, pitch);
                src += pitch;
                dst += dpitch;
            }
            if (tmp)
                delete[] tmp;
            tmp = rgb;
            fmt = TVPTextureFormat::RGB;
            pitch = dpitch;
        }
        tTVPOGLTexture2D_static* ret =
            new tTVPOGLTexture2D_static(pixel, pitch, w, h, fmt, tw, th, sw, sh);
        if (tmp)
            delete[] tmp;
        return ret;
    }

    static iTVPTexture2D* CreateStaticTexture2D_normal(const void* dib,
                                                       tjs_uint w,
                                                       tjs_uint h,
                                                       tjs_int pitch,
                                                       TVPTextureFormat::e fmt,
                                                       bool isOpaque)
    {
        int n = (w + GetMaxTextureWidth() - 1) / GetMaxTextureWidth();
        int tw = (w + n - 1) / n;
        n = (h + GetMaxTextureHeight() - 1) / GetMaxTextureHeight();
        int th = (h + n - 1) / n;
        return CreateStaticTexture2D(dib, w, h, pitch, fmt, tw, th, isOpaque);
    }

    static iTVPTexture2D* CreateStaticTexture2D_solid(
        const void* dib, tjs_uint w, tjs_uint h, tjs_int pitch, TVPTextureFormat::e fmt)
    {
        if (TVPCheckSolidPixel((const tjs_uint8*)dib, pitch, w, h))
        {
            tTVPOGLTexture2D_static* ret = new tTVPOGLTexture2D_static(
                dib, 4, 1, 1, fmt, w, h, 1.0f / w, 1.0f / h, GL_NEAREST);
            return ret;
        }
        return nullptr;
    }

    static iTVPTexture2D* CreateStaticTexture2D_half(const void* dib,
                                                     tjs_uint w,
                                                     tjs_uint h,
                                                     tjs_int pitch,
                                                     TVPTextureFormat::e fmt,
                                                     bool isOpaque)
    {
        iTVPTexture2D* ret = CreateStaticTexture2D_solid(dib, w, h, pitch, fmt);
        if (ret)
            return ret;
        int tw = w, th = h;
        if (w > GetMaxTextureWidth())
        {
            int n = (w + GetMaxTextureWidth() - 1) / GetMaxTextureWidth();
            tw = (w + n - 1) / n;
        }
        else if (w > 64)
        {
            tw = (w + 1) / 2;
        }
        if (h > GetMaxTextureHeight())
        {
            int n = (h + GetMaxTextureHeight() - 1) / GetMaxTextureHeight();
            th = (h + n - 1) / n;
        }
        else if (h > 64)
        {
            th = (h + 1) / 2;
        }
        return CreateStaticTexture2D(dib, w, h, pitch, fmt, tw, th, isOpaque);
    }

    static iTVPTexture2D* CreateStaticTexture2D(tTVPBitmap* bmp)
    {
        tjs_uint w = bmp->GetWidth(), h = bmp->GetHeight();
        if (w > GetMaxTextureWidth())
        {
            if (w > GetMaxTextureWidth() * 2 && GL_CHECK_unpack_subimage)
            {
                return new tTVPOGLTexture2D_split(bmp);
            }
        }
        else if (h > GetMaxTextureHeight() * 2)
        {
            return new tTVPOGLTexture2D_split(bmp);
        }
        return _CreateStaticTexture2D(
            bmp->GetBits(), bmp->GetWidth(), bmp->GetHeight(), bmp->GetPitch(),
            bmp->Is32bit() ? TVPTextureFormat::RGBA : TVPTextureFormat::Gray, bmp->IsOpaque);
    }

    static iTVPTexture2D* CreateMutableTexture2D(const void* pixel,
                                                 int pitch,
                                                 unsigned int w,
                                                 unsigned int h,
                                                 float sw,
                                                 float sh,
                                                 TVPTextureFormat::e fmt)
    {
        return new tTVPOGLTexture2D_mutatble(pixel, pitch, w, h, fmt, sw, sh);
    }

    static iTVPTexture2D* CreateMutableTexture2D_normal(
        const void* pixel, int pitch, unsigned int w, unsigned int h, TVPTextureFormat::e fmt)
    {
        float sw = 1.f, sh = 1.f;

        if (!pixel)
        {
            if (w > GetMaxTextureWidth())
            {
                int n = (w + GetMaxTextureWidth() - 1) / GetMaxTextureWidth();
                sw = 1.0f / n;
            }
            if (h > GetMaxTextureHeight())
            {
                int n = (h + GetMaxTextureHeight() - 1) / GetMaxTextureHeight();
                sh = 1.0f / n;
            }
        }

        return CreateMutableTexture2D(pixel, pitch, w, h, sw, sh, fmt);
    }

    static iTVPTexture2D* CreateMutableTexture2D_half(
        const void* pixel, int pitch, unsigned int w, unsigned int h, TVPTextureFormat::e fmt)
    {
        float sw = 1.f, sh = 1.f;
        if (!pixel)
        {
            if (w > GetMaxTextureWidth())
            {
                int n = (w + GetMaxTextureWidth() - 1) / GetMaxTextureWidth();
                sw = 1.0f / n;
            }
            else if (w > 64)
            {
                sw = 0.5f;
            }
            if (h > GetMaxTextureHeight())
            {
                int n = (h + GetMaxTextureHeight() - 1) / GetMaxTextureHeight();
                sh = 1.0f / n;
            }
            else if (h > 64)
            {
                sh = 0.5f;
            }
        }

        return CreateMutableTexture2D(pixel, pitch, w, h, sw, sh, fmt);
    }

    void InitGL()
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &_screenFrameBuffer);
        TVPInitTextureFormatList();

        _CreateStaticTexture2D = CreateStaticTexture2D_normal;
        _CreateMutableTexture2D = CreateMutableTexture2D_normal;
        GLint maxTextSize;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextSize);
        TVPMaxTextureSize = maxTextSize;
        if (maxTextSize > 0 && (maxTextSize < TVPMaxTextureSize || TVPMaxTextureSize < 1024))
        {
            TVPMaxTextureSize = maxTextSize; // override by user config
        }
        GLint _maxTextureUnits;
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &_maxTextureUnits);
        glGenFramebuffers(1, &_FBO);
        glGenRenderbuffers(1, &_stencil_FBO);
        if (!_FBO)
        {
            TVPConsoleLog("Fail to create FBO");
            const char* reason = nullptr;
            GLenum errcode = glCheckFramebufferStatus(GL_FRAMEBUFFER);
#define SATATUE_CASE(x) \
    case x: \
        reason = #x; \
        break;
            switch (errcode)
            {
                SATATUE_CASE(GL_FRAMEBUFFER_COMPLETE);
                SATATUE_CASE(GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT);
                SATATUE_CASE(GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT);
                SATATUE_CASE(GL_FRAMEBUFFER_UNSUPPORTED);
                default:
                {
                    char tmp[16];
                    sprintf(tmp, "0x%X", errcode);
                    TVPConsoleLog((std::string("glCheckFramebufferStatus = ") + tmp).c_str());
                }
                break;
            }
            if (reason)
            {
                TVPConsoleLog((std::string("glCheckFramebufferStatus = ") + reason).c_str());
            }
        }
    }

    tjs_uint _drawCount;

public:
    TVPRenderManager_OpenGL() : tempTexture(nullptr), _drawCount(0)
    {
        InitGL();
#ifdef TEST_SHADER_ENABLED
        TVPInitTVPGL();
#endif

        const std::string opacityPrefix("uniform float opacity;\n");
        const std::string alpha_thresholdPrefix("uniform float alpha_threshold;\n");
        const std::string colorPrefix("uniform vec4 color;\n");
        const std::string copyShader("void main(){\n"
                                     "    gl_FragColor = texture2D(tex0, v_texCoord0);"
                                     "}");
        CompileRenderMethod("Copy", copyShader.c_str(), 1);

        // ---------- ApplyColorMap ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "ApplyColorMap",
            opacityPrefix + colorPrefix + ScriptCommonPrefix +
                "    gl_FragColor = vec4(color.rgb, s.r * opacity);\n"
                "}",
            1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                   GL_ONE);
        TEST_SHADER(ApplyColorMap,
                    TVPApplyColorMap_HDA_o(testdest, testrule, 256 * 256, TEST_SHADER_COLOR,
                                           TEST_SHADER_OPA),
                    src_tex[0].first = texdata[2]);

        CompileAndRegRegularBlendMethod("ApplyColorMap_d", opacityPrefix + colorPrefix,
                                        "    s.r *= opacity;\n"
                                        "    d.a = s.r + d.a - s.r * d.a;\n"
                                        "    d.rgb = mix(d.rgb, color.rgb, s.r / (d.a + 0.0001));\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER(
            ApplyColorMap_d,
            TVPApplyColorMap_do(testdest, testrule, 256 * 256, TEST_SHADER_COLOR, TEST_SHADER_OPA),
            src_tex[0].first = texdata[2]);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("ApplyColorMap_a",
                                                        opacityPrefix + colorPrefix +
                                                            ScriptCommonPrefix +
                                                            "    s.a = s.r * opacity;\n"
                                                            "    s.rgb = color.rgb;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA);
        TEST_SHADER(
            ApplyColorMap_a,
            TVPApplyColorMap_ao(testdest, testrule, 256 * 256, TEST_SHADER_COLOR, TEST_SHADER_OPA),
            src_tex[0].first = texdata[2]);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("RemoveOpacity",
                                                        opacityPrefix + colorPrefix +
                                                            ScriptCommonPrefix +
                                                            "    s.a = s.r * opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        TEST_SHADER(RemoveOpacity,
                    TVPRemoveOpacity_o(testdest, testrule, 256 * 256, TEST_SHADER_OPA),
                    src_tex[0].first = texdata[2]);

        // ---------- Fill ----------
        std::string fillColorShader(colorPrefix + "void main(){\n"
                                                  "    gl_FragColor = color;"
                                                  "}");
#if _KRKRSDL3_GL
        CompileAndRegScript<tTVPOGLRenderMethod_FillARGB>("FillARGB", fillColorShader, 0);
        TEST_SHADER(FillARGB, TVPFillARGB(testdest, 256 * 256, TEST_SHADER_COLOR));
#endif

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("FillColor", fillColorShader, 0)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE, GL_ZERO, GL_ZERO, GL_ONE);
        TEST_SHADER(FillColor, TVPFillColor(testdest, 256 * 256, TEST_SHADER_COLOR));
        CompileAndRegScript<tTVPOGLRenderMethod_Script_BlendColor>(
            "FillMask", "void main(){gl_FragColor = vec4(0,0,0,1);}", 0)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_CONSTANT_COLOR, GL_ZERO);
        TEST_SHADER(FillMask, TVPFillMask(testdest, 256 * 256, TEST_SHADER_OPA));

        // ---------- ConstColorAlphaBlend ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script_BlendColor>("ConstColorAlphaBlend",
                                                                   fillColorShader, 0)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                                   GL_ZERO, GL_ONE);
        TEST_SHADER(
            ConstColorAlphaBlend,
            TVPConstColorAlphaBlend(testdest, 256 * 256, TEST_SHADER_COLOR, TEST_SHADER_OPA));

        CompileAndRegScript<tTVPOGLRenderMethod_Script_BlendColor>("ConstColorAlphaBlend_a",
                                                                   colorPrefix +
                                                                       "void main(){\n"
                                                                       "    vec4 s = color;\n"
                                                                       "    s.a = 1.0;\n"
                                                                       "    gl_FragColor = s;\n"
                                                                       "}",
                                                                   0)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                                   GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR);
        TEST_SHADER(
            ConstColorAlphaBlend_a,
            TVPConstColorAlphaBlend_a(testdest, 256 * 256, TEST_SHADER_COLOR, TEST_SHADER_OPA));

        const char* shader_ConstColorAlphaBlend_d =
            "    d.a = opacity + d.a - opacity * d.a;\n"
            "    d.rgb = mix(d.rgb, color.rgb, opacity / (d.a + 0.0001));\n"
            "    gl_FragColor = d;\n"
            "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "ConstColorAlphaBlend_d",
            opacityPrefix + colorPrefix +
                "void main(){\n"
                "    vec4 d = texture2D(tex0, v_texCoord0);\n" +
                shader_ConstColorAlphaBlend_d,
            1)
            ->SetTargetAsSrc();
        TEST_SHADER(
            ConstColorAlphaBlend_d, memcpy(testdest, testred, 256 * 256 * 4),
            texdest->Update(testred, TVPTextureFormat::RGBA, 256 * 4, tTVPRect(0, 0, 256, 256)),
            TVPConstColorAlphaBlend_d(testdest, 256 * 256, TEST_SHADER_COLOR, TEST_SHADER_OPA));

        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "RemoveConstOpacity",
            opacityPrefix + "void main(){\n"
                            "    gl_FragColor = vec4(0,0,0,opacity);\n"
                            "}",
            0)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        TEST_SHADER(RemoveConstOpacity,
                    TVPRemoveConstOpacity(testdest, 256 * 256, TEST_SHADER_OPA));

        // ---------- Copy ----------
        CompileRenderMethod("CopyOpaqueImage",
                            "void main(){\n"
                            "    vec4 s = texture2D(tex0, v_texCoord0);\n"
                            "    s.a = 1.0;\n"
                            "    gl_FragColor = s;\n"
                            "}",
                            1);
        TEST_SHADER(CopyOpaqueImage, TVPCopyOpaqueImage(testdest, testdata1, 256 * 256));
        CompileAndRegScript<tTVPOGLRenderMethod_Script>("CopyColor", copyShader, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE, GL_ZERO, GL_ZERO, GL_ONE);
        TEST_SHADER(CopyColor, TVPCopyColor(testdest, testdata1, 256 * 256));
        CompileAndRegScript<tTVPOGLRenderMethod_Script>("CopyMask", copyShader, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
        TEST_SHADER(CopyMask, TVPCopyMask(testdest, testdata1, 256 * 256));

        // ---------- ConstAlphaBlend ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script_BlendColor>("ConstAlphaBlend", copyShader, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                                   GL_ZERO, GL_ONE);
        TEST_SHADER(ConstAlphaBlend,
                    TVPConstAlphaBlend_HDA(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("ConstAlphaBlend_a",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.a = opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA);
        TEST_SHADER(ConstAlphaBlend_a,
                    TVPConstAlphaBlend_a(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        CompileAndRegRegularBlendMethod("ConstAlphaBlend_d", opacityPrefix,
                                        "    d.a = opacity + d.a - opacity * d.a;\n"
                                        "    d.rgb = mix(d.rgb, s.rgb, opacity / (d.a + 0.0001));\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER(ConstAlphaBlend_d,
                    TVPConstAlphaBlend_d(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        // ---------- AdjustGamma ----------
        CompileAndRegScript<tTVPOGLRenderMethod_AdjustGamma>(
            "AdjustGamma",
            "uniform vec3 u_gamma;\n" // rgb
            "uniform vec3 u_floor;\n" // rgb
            "uniform vec3 u_amp;\n"   // rgb
            "void main()\n"
            "{\n"
            "    vec4 s = texture2D(tex0, v_texCoord0);\n"
            "    s.rgb = clamp(pow(s.rgb, u_gamma) * u_amp + u_floor, 0.0, 1.0);\n"
            "    gl_FragColor = s;\n"
            "}",
            1)
            ->SetTargetAsSrc();
#ifdef TEST_SHADER_ENABLED
        tTVPGLGammaAdjustTempData temp;
        tTVPGLGammaAdjustData data;
        data.BCeil = 220;
        data.BFloor = 20;
        data.BGamma = 0.5;
        data.RCeil = 210;
        data.RFloor = 10;
        data.RGamma = 1.5;
        data.GCeil = 240;
        data.GFloor = 50;
        data.GGamma = 6.0;
        TVPInitGammaAdjustTempData(&temp, &data);
#endif
        TEST_SHADER(AdjustGamma, TVPAdjustGamma(testdest, 256 * 256, &temp),
                    method->SetParameterPtr(method->EnumParameterID("gammaAdjustData"), &data));

        std::string shader_AdjustGamma_a_1 = "uniform vec3 u_gamma;\n" // rgb
                                             "uniform vec3 u_floor;\n" // rgb
                                             "uniform vec3 u_amp;\n"   // rgb
                                             "void main() {\n";
        std::string shader_AdjustGamma_a_2 =
            "    vec3 t = (s.rgb / (s.a + 0.001) - 1.0) * step(s.rgb, s.aaa + 0.001) + 1.0;\n"
            "    t = clamp(pow(t, u_gamma) * u_amp + u_floor, 0.0, 1.0);\n"
            "    s.rgb = t * s.a + clamp(s.rgb - s.a, 0.0, 1.0);\n"
            "    gl_FragColor = s;\n"
            "}";
            CompileAndRegScript<tTVPOGLRenderMethod_AdjustGamma>(
                "AdjustGamma_a",
                shader_AdjustGamma_a_1 + "    vec4 s = texture2D(tex0, v_texCoord0);\n" +
                    shader_AdjustGamma_a_2,
                1)
                ->SetTargetAsSrc();

        // --------- ConstAlphaBlend_SD ----------
        tTVPOGLRenderMethod_Script* method;
        method = CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "ConstAlphaBlend_SD",
            opacityPrefix + ScriptCommonBltPrefix +
                "    gl_FragColor = mix(s, d, opacity);\n"
                "}",
            2);
        TEST_SHADER_IGNORE_ALPHA(
            ConstAlphaBlend_SD,
            TVPConstAlphaBlend_SD(testdest, testdata1, testdata2, 256 * 256, TEST_SHADER_OPA));
        RegisterRenderMethod("ConstAlphaBlend_SD_a", method);
        TEST_SHADER(ConstAlphaBlend_SD_a, TVPConstAlphaBlend_SD_a(testdest, testdata1, testdata2,
                                                                  256 * 256, TEST_SHADER_OPA));
        CompileRenderMethod(
            "ConstAlphaBlend_SD_d",
            (opacityPrefix + ScriptCommonBltPrefix +
             "    s.rgb = mix(s.rgb, d.rgb, d.a * opacity / (s.a * (1.0 - opacity) * (1.0 - d.a * "
             "opacity) + d.a * opacity + 0.0001));\n" // mix(x,y,a)=x*(1-a)+y*a
             "    s.a = mix(s.a, d.a, opacity);\n"
             "    gl_FragColor = s;\n"
             "}")
                .c_str(),
            2);
        TEST_SHADER(ConstAlphaBlend_SD_d, TVPConstAlphaBlend_SD_d(testdest, testdata1, testdata2,
                                                                  256 * 256, TEST_SHADER_OPA));
        // ConstAlphaBlend_SD = ConstAlphaBlend
        // --------- UnivTransBlend ----------
        method = CompileAndRegScript<tTVPOGLRenderMethod_UnivTrans>(
            "UnivTransBlend",
            "uniform float phase;\n" // = phase - vague
            "uniform float vague;\n"
            "void main()\n"
            "{\n"
            "    vec4 s1 = texture2D(tex0, v_texCoord0);\n"
            "    vec4 s2 = texture2D(tex1, v_texCoord1);\n"
            "    vec4 rule = texture2D(tex2, v_texCoord2);\n"
            "    float opa = clamp((rule.r - phase) / vague, 0.0, 1.0);\n"
            "    s1.rgb = mix(s2.rgb, s1.rgb, opa);\n"
            "    gl_FragColor = s1;\n"
            "}",
            3);
        TEST_SHADER_IGNORE_ALPHA(
            UnivTransBlend, TVPInitUnivTransBlendTable(testtable, 64, 128),
            TVPUnivTransBlend(testdest, testdata1, testdata2, testrule, testtable, 256 * 256),
            method->SetParameterInt(method->EnumParameterID("vague"), 128),
            method->SetParameterInt(method->EnumParameterID("phase"), 64));
        TEST_SHADER_IGNORE_ALPHA(
            UnivTransBlend, TVPInitUnivTransBlendTable(testtable, 192, 128),
            TVPUnivTransBlend(testdest, testdata1, testdata2, testrule, testtable, 256 * 256),
            method->SetParameterInt(method->EnumParameterID("phase"), 192));
        TEST_SHADER_IGNORE_ALPHA(
            UnivTransBlend, TVPInitUnivTransBlendTable(testtable, 64 + 600, 600),
            TVPUnivTransBlend_switch(testdest, testdata1, testdata2, testrule, testtable, 256 * 256,
                                     64 + 600, 64),
            method->SetParameterInt(method->EnumParameterID("vague"), 600),
            method->SetParameterInt(method->EnumParameterID("phase"), 64 + 600));

        method = CompileAndRegScript<tTVPOGLRenderMethod_UnivTrans>(
            "UnivTransBlend_d",
            "uniform float phase;\n" // = phase - vague
            "uniform float vague;\n"
            "void main()\n"
            "{\n"
            "    vec4 s1 = texture2D(tex0, v_texCoord0);\n"
            "    vec4 s2 = texture2D(tex1, v_texCoord1);\n"
            "    vec4 rule = texture2D(tex2, v_texCoord2);\n"
            "    float opa = clamp((rule.r - phase) / vague, 0.0, 1.0);\n"
            "    s1.rgb = mix(s1.rgb, s2.rgb, s2.a * (1.0 - opa) / (s2.a * (1.0 - opa) * (1.0 - "
            "s1.a * opa) + s1.a * opa + 0.0001));\n" // mix(x,y,a)=x*(1-a)+y*a
            "    s1.a = mix(s2.a, s1.a, opa);\n"
            "    gl_FragColor = s1;\n"
            "}",
            3);
        TEST_SHADER(
            UnivTransBlend_d, TVPInitUnivTransBlendTable(testtable, 215, 128),
            TVPUnivTransBlend_d(testdest, testdata1, testdata2, testrule, testtable, 256 * 256),
            method->SetParameterInt(method->EnumParameterID("vague"), 128),
            method->SetParameterInt(method->EnumParameterID("phase"), 215));
        TEST_SHADER(UnivTransBlend_d,
                    (TVPInitUnivTransBlendTable(testtable, 215 + 600, 600),
                     TVPUnivTransBlend_switch_d(testdest, testdata1, testdata2, testrule, testtable,
                                                256 * 256, 215 + 600, 215)),
                    method->SetParameterInt(method->EnumParameterID("vague"), 600),
                    method->SetParameterInt(method->EnumParameterID("phase"), 215 + 600));

        method = CompileAndRegScript<tTVPOGLRenderMethod_UnivTrans>(
            "UnivTransBlend_a",
            "uniform float phase;\n" // = phase - vague
            "uniform float vague;\n"
            "void main()\n"
            "{\n"
            "    vec4 s1 = texture2D(tex0, v_texCoord0);\n"
            "    vec4 s2 = texture2D(tex1, v_texCoord1);\n"
            "    vec4 rule = texture2D(tex2, v_texCoord2);\n"
            "    float opa = clamp((rule.r - phase) / vague, 0.0, 1.0);\n"
            "    gl_FragColor = mix(s2, s1, opa);\n"
            "}",
            3);
        TEST_SHADER(
            UnivTransBlend_a,
            (TVPInitUnivTransBlendTable(testtable, 215, 128),
             TVPUnivTransBlend_a(testdest, testdata1, testdata2, testrule, testtable, 256 * 256)),
            method->SetParameterInt(method->EnumParameterID("vague"), 128),
            method->SetParameterInt(method->EnumParameterID("phase"), 215));
        TEST_SHADER(
            UnivTransBlend_a,
            (TVPInitUnivTransBlendTable(testtable, 215, 600),
             TVPUnivTransBlend_a(testdest, testdata1, testdata2, testrule, testtable, 256 * 256)),
            method->SetParameterInt(method->EnumParameterID("vague"), 600),
            method->SetParameterInt(method->EnumParameterID("phase"), 215));

        // --------- AlphaBlend ----------
#define TEST_SHADER_BLTO(x) \
    TEST_SHADER(x, TVP##x##_HDA_o(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));
        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AlphaBlend",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.a *= opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                   GL_ONE);
        TEST_SHADER_BLTO(AlphaBlend);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AlphaBlend_color",
                                                        colorPrefix + ScriptCommonPrefix +
                                                            "    s *= color;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                   GL_ONE);

#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegScript<tTVPOGLRenderMethod_AlphaTest>("AlphaBlend_color_AlphaTest",
                                                               colorPrefix + ScriptCommonPrefix +
                                                                   "    s *= color;\n"
                                                                   "    gl_FragColor = s;\n"
                                                                   "}",
                                                               1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                       GL_ONE);
            CompileAndRegScript<tTVPOGLRenderMethod_AlphaTest>("AlphaTest",
                                                               ScriptCommonPrefix +
                                                                   "    gl_FragColor = s;\n"
                                                                   "}",
                                                               1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
        }
        else
#endif
        {
            CompileAndRegScript<tTVPOGLRenderMethod_Script>(
                "AlphaBlend_color_AlphaTest",
                colorPrefix + alpha_thresholdPrefix + ScriptCommonPrefix +
                    "    s *= color;\n"
                    "    if(s.a < alpha_threshold) discard;\n"
                    "    else gl_FragColor = s;\n"
                    "}",
                1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                       GL_ONE);
            CompileAndRegScript<tTVPOGLRenderMethod_Script>(
                "AlphaTest",
                alpha_thresholdPrefix + ScriptCommonPrefix +
                    "    if(s.a < alpha_threshold) discard;\n"
                    "    gl_FragColor = s;\n"
                    "}",
                1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_ZERO, GL_ONE);
        }

        CompileAndRegScript<tTVPOGLRenderMethod_Script_BlendColor>(
            "AlphaBlend_SD", /*opacityPrefix +*/ ScriptCommonPrefix + "    gl_FragColor = s;\n"
                                                                      "}",
            1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
                                   GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR);
        TEST_SHADER_BLTO(AlphaBlend);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("PsAlphaBlend",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.a *= opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO,
                                   GL_ONE);
        TEST_SHADER_BLTO(PsAlphaBlend);

        CompileAndRegScript<tTVPOGLRenderMethod_Perspective>(
            "PerspectiveAlphaBlend_a",
            opacityPrefix + "uniform mat3 M;\n"
                            "void main() {\n"
                            "    vec3 S = vec3(gl_FragCoord.xy, 1) * M;\n"
                            "    vec4 s = texture2D(tex0, S.xy / S.z);\n"
                            "    s.a *= opacity;\n"
                            "    gl_FragColor = s;\n"
                            "}",
            1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AlphaBlend_a",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.a *= opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA);
        TEST_SHADER(AlphaBlend_a,
                    TVPAlphaBlend_ao(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AlphaBlend_color_a",
                                                        colorPrefix + ScriptCommonPrefix +
                                                            "    s *= color;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA);

#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegScript<tTVPOGLRenderMethod_AlphaTest>("AlphaBlend_color_a_AlphaTest",
                                                               colorPrefix + ScriptCommonPrefix +
                                                                   "    s *= color;\n"
                                                                   "    gl_FragColor = s;\n"
                                                                   "}",
                                                               1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                       GL_ONE_MINUS_SRC_ALPHA);
        }
        else
#endif
        {
            CompileAndRegScript<tTVPOGLRenderMethod_Script>(
                "AlphaBlend_color_a_AlphaTest",
                colorPrefix + alpha_thresholdPrefix + ScriptCommonPrefix +
                    "    s *= color;\n"
                    "    if(s.a < alpha_threshold) discard;\n"
                    "    gl_FragColor = s;\n"
                    "}",
                1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                       GL_ONE_MINUS_SRC_ALPHA);
        }

        const char* shader_AlphaBlend_d = "    s.a *= opacity;\n"
                                          "    d.a = s.a + d.a - s.a * d.a;\n"
                                          "    d.rgb = mix(d.rgb, s.rgb, s.a / (d.a + 0.0001));\n"
                                          "    gl_FragColor = d;\n"
                                          "}";
        CompileAndRegRegularBlendMethod("AlphaBlend_d", opacityPrefix, shader_AlphaBlend_d);
        TEST_SHADER(AlphaBlend_d,
                    TVPAlphaBlend_do(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        shader_AlphaBlend_d = "    s *= color;\n"
                              "    d.a = s.a + d.a - s.a * d.a;\n"
                              "    d.rgb = mix(d.rgb, s.rgb, s.a / (d.a + 0.0001));\n"
                              "    gl_FragColor = d;\n"
                              "}";
        CompileAndRegRegularBlendMethod("AlphaBlend_color_d", colorPrefix, shader_AlphaBlend_d);
#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegRegularBlendMethod<tTVPOGLRenderMethod_AlphaTest>(
                "AlphaBlend_color_d_AlphaTest", colorPrefix, shader_AlphaBlend_d);
        }
        else
#endif
        {
            CompileAndRegRegularBlendMethod("AlphaBlend_color_d_AlphaTest",
                                            colorPrefix + alpha_thresholdPrefix,
                                            "    s *= color;\n"
                                            "    if(s.a < alpha_threshold) discard;\n"
                                            "    d.a = s.a + d.a - s.a * d.a;\n"
                                            "    d.rgb = mix(d.rgb, s.rgb, s.a / (d.a + 0.0001));\n"
                                            "    gl_FragColor = d;\n"
                                            "}");
        }

        // --------- AddBlend ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script_BlendColor>("AddBlend", copyShader, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_CONSTANT_COLOR, GL_ONE, GL_ZERO, GL_ONE);
        TEST_SHADER_BLTO(AddBlend);

        // --------- SubBlend ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script>("SubBlend",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.rgb = (1.0 - s.rgb) * opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
        TEST_SHADER_BLTO(SubBlend);

        // --------- MulBlend ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script>("MulBlend",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.rgb = (1.0 - s.rgb) * opacity;\n"
                                                            "    s.a = 0.0;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
        // TEST_SHADER_BLTO(MulBlend);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("MulBlend_HDA",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.rgb = (1.0 - s.rgb) * opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
        // TEST_SHADER_BLTO(MulBlend_HDA);

        // --------- AdditiveAlpha ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AdditiveAlphaBlend",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    gl_FragColor = s * opacity;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        TEST_SHADER_BLTO(AdditiveAlphaBlend);

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AdditiveAlphaBlend_a",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s *= opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                   GL_ONE_MINUS_SRC_ALPHA);
        TEST_SHADER(AdditiveAlphaBlend_a,
                    TVPAdditiveAlphaBlend_ao(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        // --------- ColorDodgeBlend ----------
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "ColorDodgeBlend",
            opacityPrefix + ScriptCommonBltPrefix +
                //"    d.rgb /= (1.0 - s.rgb * opacity);\n"
                //"    gl_FragColor = d;\n"
                "    s.rgb = 1.0 / (1.0 - s.rgb * opacity);"
                "    gl_FragColor = s;\n"
                "}",
            2)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        // TEST_SHADER_BLTO(ColorDodgeBlend); pass it

        CompileAndRegRegularBlendMethod("DarkenBlend", opacityPrefix,
                                        "    d.rgb = mix(d.rgb, min(s.rgb, d.rgb), opacity);\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER_BLTO(DarkenBlend);

        CompileAndRegRegularBlendMethod("LightenBlend", opacityPrefix,
                                        "    d.rgb = mix(d.rgb, max(s.rgb, d.rgb), opacity);\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER(LightenBlend,
                    TVPLightenBlend_HDA_o(testdest, testdata1, 256 * 256, TEST_SHADER_OPA));

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("ScreenBlend",
                                                        opacityPrefix + ScriptCommonPrefix +
                                                            "    s.rgb *= opacity;\n"
                                                            "    gl_FragColor = s;\n"
                                                            "}",
                                                        1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
        TEST_SHADER_BLTO(ScreenBlend);

        // --------- PsXxxBlend ----------
        const char* shader_PsAddBlend =
            "    d.rgb = mix(d.rgb, clamp(s.rgb + d.rgb, 0.0, 1.0), s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}";
        CompileAndRegRegularBlendMethod("PsAddBlend", opacityPrefix, shader_PsAddBlend);
        TEST_SHADER_BLTO(PsAddBlend);

        shader_PsAddBlend = "    s *= color;\n"
                            "    d.rgb = mix(d.rgb, clamp(s.rgb + d.rgb, 0.0, 1.0), s.a);\n"
                            "    gl_FragColor = d;\n"
                            "}";
        CompileAndRegRegularBlendMethod("PsAddBlend_color", colorPrefix, shader_PsAddBlend);
#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegRegularBlendMethod<tTVPOGLRenderMethod_AlphaTest>(
                "PsAddBlend_color_AlphaTest", colorPrefix, shader_PsAddBlend);
        }
        else
#endif
        {
            CompileAndRegRegularBlendMethod(
                "PsAddBlend_color_AlphaTest", colorPrefix + alpha_thresholdPrefix,
                "    s *= color;\n"
                "    if(s.a < alpha_threshold) discard;\n"
                "    d.rgb = mix(d.rgb, clamp(s.rgb + d.rgb, 0.0, 1.0), s.a);\n"
                "    gl_FragColor = d;\n"
                "}");
        }

        const char* shader_PsSubBlend =
            "    d.rgb = mix(d.rgb, clamp(d.rgb + s.rgb - 1.0, 0.0, 1.0), s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}";
        CompileAndRegRegularBlendMethod("PsSubBlend", opacityPrefix, shader_PsSubBlend);
        TEST_SHADER_BLTO(PsSubBlend);

        shader_PsSubBlend = "    s *= color;\n"
                            "    d.rgb = mix(d.rgb, clamp(d.rgb + s.rgb - 1.0, 0.0, 1.0), s.a);\n"
                            "    gl_FragColor = d;\n"
                            "}";
        CompileAndRegRegularBlendMethod("PsSubBlend_color", colorPrefix, shader_PsSubBlend);
#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegRegularBlendMethod<tTVPOGLRenderMethod_AlphaTest>(
                "PsSubBlend_color_AlphaTest", colorPrefix, shader_PsSubBlend);
        }
        else
#endif
        {
            CompileAndRegRegularBlendMethod(
                "PsSubBlend_color_AlphaTest", colorPrefix + alpha_thresholdPrefix,
                "    s *= color;\n"
                "    if(s.a < alpha_threshold) discard;\n"
                "    d.rgb = mix(d.rgb, clamp(d.rgb + s.rgb - 1.0, 0.0, 1.0), s.a);\n"
                "    gl_FragColor = d;\n"
                "}");
        }

        const char* shader_PsMulBlend = "    s.a *= opacity;\n"
                                        "    s.rgb *= s.a;\n"
                                        "    s.rgb += 1.0 - s.a;\n"
                                        "    gl_FragColor = s;\n"
                                        "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "PsMulBlend", opacityPrefix + ScriptCommonPrefix + shader_PsMulBlend, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        TEST_SHADER_BLTO(PsMulBlend);

        shader_PsMulBlend = "    s *= color;\n"
                            "    s.rgb *= s.a;\n"
                            "    s.rgb += 1.0 - s.a;\n"
                            "    gl_FragColor = s;\n"
                            "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "PsMulBlend_color", colorPrefix + ScriptCommonPrefix + shader_PsMulBlend, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegScript<tTVPOGLRenderMethod_AlphaTest>(
                "PsMulBlend_color_AlphaTest", colorPrefix + ScriptCommonPrefix + shader_PsMulBlend,
                1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        }
        else
#endif
        {
            CompileAndRegScript<tTVPOGLRenderMethod_Script>(
                "PsMulBlend_color_AlphaTest",
                colorPrefix + alpha_thresholdPrefix + ScriptCommonPrefix +
                    "    s *= color;\n"
                    "    if(s.a < alpha_threshold) discard;\n"
                    "    s.rgb *= s.a;\n"
                    "    s.rgb += 1.0 - s.a;\n"
                    "    gl_FragColor = s;\n"
                    "}",
                1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_SRC_COLOR, GL_ZERO, GL_ONE);
        }

        CompileAndRegRegularBlendMethod(
            "PsOverlayBlend", opacityPrefix,
            "    vec3 sd = s.rgb * d.rgb;\n"
            "    s.rgb = step(0.5, d.rgb) * ((s.rgb + d.rgb) * 2.0 - sd * 4.0 - 1.0) + sd * 2.0;\n"
            "    d.rgb = mix(d.rgb, s.rgb, s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsOverlayBlend);

        CompileAndRegRegularBlendMethod(
            "PsHardLightBlend", opacityPrefix,
            "    vec3 sd = s.rgb * d.rgb;\n"
            "    s.rgb = step(0.5, s.rgb) * ((s.rgb + d.rgb) * 2.0 - sd * 4.0 - 1.0) + sd * 2.0;\n"
            "    d.rgb = mix(d.rgb, s.rgb, s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsHardLightBlend);

        CompileAndRegRegularBlendMethod(
            "PsSoftLightBlend", opacityPrefix,
            "    vec3 t = 0.5 / (s.rgb + 0.001);\n" // avoid n / 0
            "    s.rgb = pow(d.rgb, (1.0 - step(0.5, s.rgb)) * ((1.0 - s.rgb) * 2.0 - t) + t);\n"
            "    d.rgb = mix(d.rgb, s.rgb, s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsSoftLightBlend);

        CompileAndRegRegularBlendMethod("PsColorDodgeBlend", opacityPrefix,
                                        "    s.rgb -= 0.001;\n" // avoid n / 0
                                        "    s.rgb = d.rgb / max(1.0 - s.rgb, d.rgb);\n"
                                        "    d.rgb = mix(d.rgb, s.rgb, s.a * opacity);\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        // TEST_SHADER_BLTO(PsColorDodgeBlend);
        // pass

        CompileAndRegRegularBlendMethod("PsColorDodge5Blend", opacityPrefix,
                                        "    s.rgb = s.rgb * s.a * opacity - 0.001;\n"
                                        "    d.rgb = d.rgb / max(1.0 - s.rgb, d.rgb);\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER_BLTO(PsColorDodge5Blend);

        CompileAndRegRegularBlendMethod("PsColorBurnBlend", opacityPrefix,
                                        "    vec3 t = 1.0 - d.rgb;\n"
                                        "    s.rgb = step(t + 0.001, s.rgb) * (1.0 - t / s.rgb);\n"
                                        "    s.rgb = clamp(s.rgb, 0.0, 1.0);\n"
                                        "    d.rgb = mix(d.rgb, s.rgb, s.a * opacity);\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER_BLTO(PsColorBurnBlend);

        CompileAndRegRegularBlendMethod(
            "PsLightenBlend", opacityPrefix,
            "    d.rgb = mix(d.rgb, max(s.rgb, d.rgb), s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsLightenBlend);

        CompileAndRegRegularBlendMethod(
            "PsDarkenBlend", opacityPrefix,
            "    d.rgb = mix(d.rgb, min(s.rgb, d.rgb), s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsDarkenBlend);

        CompileAndRegRegularBlendMethod(
            "PsDiffBlend", opacityPrefix,
            "    d.rgb = mix(d.rgb, abs(s.rgb - d.rgb), s.a * opacity);\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsDiffBlend);

        CompileAndRegRegularBlendMethod("PsDiff5Blend", opacityPrefix,
                                        "    s.rgb *= s.a * opacity;\n"
                                        "    d.rgb = abs(s.rgb - d.rgb);\n"
                                        "    gl_FragColor = d;\n"
                                        "}");
        TEST_SHADER_BLTO(PsDiff5Blend);

        CompileAndRegRegularBlendMethod(
            "PsExclusionBlend", opacityPrefix,
            "    d.rgb += (s.rgb - (s.rgb * d.rgb * 2.0)) * s.a * opacity;\n"
            "    gl_FragColor = d;\n"
            "}");
        TEST_SHADER_BLTO(PsExclusionBlend);

        const char* shader_PsScreenBlend = "    s.rgb *= opacity * s.a;\n"
                                           "    gl_FragColor = s;\n"
                                           "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "PsScreenBlend", opacityPrefix + ScriptCommonPrefix + shader_PsScreenBlend, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
        TEST_SHADER_BLTO(PsScreenBlend);

        shader_PsScreenBlend = "    s *= color;\n"
                               "    s.rgb *= s.a;\n"
                               "    gl_FragColor = s;\n"
                               "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "PsScreenBlend_color", colorPrefix + ScriptCommonPrefix + shader_PsScreenBlend, 1)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ZERO, GL_ONE);
#if _KRKRSDL3_GL
        if (glAlphaFunc)
        {
            CompileAndRegScript<tTVPOGLRenderMethod_AlphaTest>(
                "PsScreenBlend_color_AlphaTest",
                colorPrefix + ScriptCommonPrefix + shader_PsScreenBlend, 1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ZERO,
                                       GL_ONE);
        }
        else
#endif
        {
            CompileAndRegScript<tTVPOGLRenderMethod_Script>(
                "PsScreenBlend_color_AlphaTest",
                colorPrefix + alpha_thresholdPrefix + ScriptCommonPrefix +
                    "    s *= color;\n"
                    "    if(s.a < alpha_threshold) discard;\n"
                    "    s.rgb *= s.a;\n"
                    "    gl_FragColor = s;\n"
                    "}",
                1)
                ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ONE_MINUS_DST_COLOR, GL_ONE, GL_ZERO,
                                       GL_ONE);
        }
        // AdditiveAlpha <-> Alpha
        const char* shader_AdditiveAlphaToAlpha = "    gl_FragColor = vec4(s.rgb / s.a, s.a);"
                                                  "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "AdditiveAlphaToAlpha", ScriptCommonPrefix + shader_AdditiveAlphaToAlpha, 1)
            ->SetTargetAsSrc();
        TEST_SHADER(AdditiveAlphaToAlpha, TVPConvertAdditiveAlphaToAlpha(testdest, 256 * 256));

        CompileAndRegScript<tTVPOGLRenderMethod_Script>("AlphaToAdditiveAlpha", "void main(){}", 0)
            ->SetBlendFuncSeparate(GL_FUNC_ADD, GL_ZERO, GL_DST_ALPHA, GL_ZERO, GL_ONE);
        TEST_SHADER(AlphaToAdditiveAlpha, TVPConvertAlphaToAdditiveAlpha(testdest, 256 * 256));

        // DoGrayScale
        const char* shader_DoGrayScale =
            "    float c = s.r * 0.0743 + s.g * 0.7148 + s.b * 0.2109;\n"
            "    s.r = c; s.g = c; s.b = c;\n"
            "    gl_FragColor = s;\n"
            "}";
        CompileAndRegScript<tTVPOGLRenderMethod_Script>(
            "DoGrayScale", ScriptCommonPrefix + shader_DoGrayScale, 1)
            ->SetTargetAsSrc();
        TEST_SHADER(DoGrayScale, TVPDoGrayScale(testdest, 256 * 256));

        CompileAndRegScript<tTVPOGLRenderMethod_BoxBlur>(
            "BoxBlur",
            "uniform vec2 u_sample[8];\n"
            "void main()\n"
            "{\n"
            "    vec4 s = texture2D(tex0, v_texCoord0);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[0]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[1]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[2]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[3]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[4]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[5]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[6]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[7]);\n"
            "    gl_FragColor = s / vec4(9, 9, 9, 9);\n"
            "}",
            1) /*->SetTargetAsSrc()*/;
        CompileAndRegScript<tTVPOGLRenderMethod_BoxBlur>(
            "BoxBlurAlpha",
            "uniform vec2 u_sample[8];\n"
            "void main()\n"
            "{\n"
            "    vec4 s = texture2D(tex0, v_texCoord0);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[0]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[1]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[2]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[3]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[4]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[5]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[6]);\n"
            "    s += texture2D(tex0, v_texCoord0 + u_sample[7]);\n"
            "    gl_FragColor = s / vec4(9, 9, 9, 9);\n"
            "}",
            1) /*->SetTargetAsSrc()*/;
    }

    tTVPOGLTexture2D* tempTexture;
    tTVPOGLTexture2D* GetTempTexture2D(tTVPOGLTexture2D* src, const tTVPRect& rcsrc)
    {
        unsigned int w = rcsrc.get_width(), h = rcsrc.get_height();
        if (!tempTexture || tempTexture->internalW < w || tempTexture->internalH < h)
        {
            if (tempTexture)
                tempTexture->Release();
            tempTexture =
                new tTVPOGLTexture2D_mutatble(nullptr, 0, w, h, TVPTextureFormat::RGBA, 1.f, 1.f);
            // tempTexture = (tTVPOGLTexture2D *)CreateTexture2D(nullptr, 0, w, h,
            // TVPTextureFormat::RGBA);
        }
        tempTexture->Width = w;
        tempTexture->Height = h;
        CopyTexture(tempTexture, src, rcsrc);
        return tempTexture;
    }

    virtual iTVPTexture2D* CreateTexture2D(const void* pixel,
                                           int pitch,
                                           unsigned int w,
                                           unsigned int h,
                                           TVPTextureFormat::e format,
                                           int flags) override
    {
        if (pixel || (flags & RENDER_CREATE_TEXTURE_FLAG_STATIC))
        {
            if (!pixel || (flags & RENDER_CREATE_TEXTURE_FLAG_NO_COMPRESS))
            {
                return CreateStaticTexture2D(pixel, w, h, pitch, format, w, h, false);
            }
            return _CreateStaticTexture2D(pixel, w, h, pitch, format, false);
        }
        return _CreateMutableTexture2D(pixel, pitch, w, h, format);
    }

    virtual iTVPTexture2D* CreateTexture2D(tTVPBitmap* bmp) override
    {
        tjs_uint w = bmp->GetWidth(), h = bmp->GetHeight();
        if (w > GetMaxTextureWidth())
        {
            if (w > GetMaxTextureWidth() * 2 && GL_CHECK_unpack_subimage)
            {
                return new tTVPOGLTexture2D_split(bmp);
            }
        }
        else if (h > GetMaxTextureHeight() * 2)
        {
            return new tTVPOGLTexture2D_split(bmp);
        }
        return _CreateStaticTexture2D(
            bmp->GetBits(), bmp->GetWidth(), bmp->GetHeight(), bmp->GetPitch(),
            bmp->Is32bit() ? TVPTextureFormat::RGBA : TVPTextureFormat::Gray, bmp->IsOpaque);
    }

    virtual iTVPTexture2D* CreateTexture2D(unsigned int neww,
                                           unsigned int newh,
                                           iTVPTexture2D* tex) override
    {
        tTVPOGLTexture2D* newtex = static_cast<tTVPOGLTexture2D*>(
            _CreateMutableTexture2D(nullptr, 0, neww, newh, tex->GetFormat()));
        CopyTexture(newtex, static_cast<tTVPOGLTexture2D*>(tex),
                    tTVPRect(0, 0, tex->GetWidth(), tex->GetHeight()));
        return newtex;
    }

    virtual iTVPTexture2D* CreateTexture2D(tTJSBinaryStream* src) override { return nullptr; }

    void CopyTexture(tTVPOGLTexture2D* dst, tTVPOGLTexture2D* src, const tTVPRect& rcsrc)
    {
#if _KRKRSDL3_GL
        if (glCopyImageSubData && !src->IsCompressed && src->_scaleW == dst->_scaleW &&
            src->_scaleH == dst->_scaleH && src->Format == dst->Format)
        {
            tTVPRect rc;
            rc.left = rcsrc.left * src->_scaleW + 0.5f;
            rc.right = rcsrc.right * src->_scaleW;
            rc.top = rcsrc.top * src->_scaleH + 0.5f;
            rc.bottom = rcsrc.bottom * src->_scaleH;
            tTVPRect rcsrc;
            rcsrc.left = 0;
            rcsrc.top = 0;
            rcsrc.right = src->GetInternalWidth();
            rcsrc.bottom = src->GetInternalHeight();
            TVPIntersectRect(&rc, rc, rcsrc);
            if (dst->GetInternalWidth() < rc.get_width())
            {
                rc.set_width(dst->GetInternalWidth());
            }
            if (dst->GetInternalHeight() < rc.get_height())
            {
                rc.set_height(dst->GetInternalHeight());
            }
            if (rc.get_width() == 0 || rc.get_height() == 0)
                return;
            glCopyImageSubData(src->texture, GL_TEXTURE_2D, 0, rc.left, rc.top, 0, dst->texture,
                                   GL_TEXTURE_2D, 0, 0, 0, 0, rc.get_width(), rc.get_height(), 1);
            return;
        }
#endif

        static tTVPOGLRenderMethod* method = (tTVPOGLRenderMethod*)GetRenderMethod("Copy");
        static const GLfloat minx = -1, maxx = 1, miny = -1, maxy = 1;
        static const GLfloat vertices[12] = {minx, miny, maxx, miny, minx, maxy,

                                             maxx, miny, minx, maxy, maxx, maxy};
        method->Apply();
        dst->AsTarget();
        float sw, sh;
        dst->GetScale(sw, sh);
        glViewport(0, 0, rcsrc.get_width() * sw, rcsrc.get_height() * sh);
        int VA_flag = 1 << method->GetPosAttr();
        for (unsigned int i = 0; i < 1; ++i)
        {
            VA_flag |= 1 << method->GetTexCoordAttr(i);
        }
        _glEnableVertexAttribs(VA_flag);
        glVertexAttribPointer(method->GetPosAttr(), 2, GL_FLOAT, GL_FALSE, 0, vertices);

        GLVertexInfo vtx;
        src->ApplyVertex(vtx, rcsrc);
        glBindTexture(GL_TEXTURE_2D, src->texture);

        glVertexAttribPointer(method->GetTexCoordAttr(0), 2, GL_FLOAT, GL_FALSE, 0,
                              &vtx.vtx.front());
        glDrawArrays(GL_TRIANGLES, 0, 6);
        method->onFinish();
    }

    virtual const char* GetName() override { return "OpenGL"; }
    virtual bool IsSoftware() override { return false; }
    virtual bool GetRenderStat(unsigned int& drawCount, uint64_t& vmemsize) override
    {
        drawCount = _drawCount;
        _drawCount = 0;
        vmemsize = _totalVMemSize;

        return true;
    }

    virtual bool GetTextureStat(iTVPTexture2D* texture, uint64_t& vmemsize) override
    {
        if (!texture)
        {
            vmemsize = 0;
            return false;
        }
        vmemsize = texture->GetInternalWidth() * texture->GetInternalHeight() * 4;
        return true;
    }

    // dst x Tex1 x ... x TexN -> dst
    virtual void OperateRect(iTVPRenderMethod* _method,
                             iTVPTexture2D* _tar,
                             iTVPTexture2D* reftar,
                             const tTVPRect& rctar,
                             const tRenderTexRectArray& textures) override
    {
        ++_drawCount;
        tTVPOGLRenderMethod* method = (tTVPOGLRenderMethod*)_method;
        tTVPOGLTexture2D* tar = (tTVPOGLTexture2D*)_tar;
        if (reftar == _tar)
            reftar = nullptr;
        if (method->CustomProc &&
            method->CustomProc(method, tar, (tTVPOGLTexture2D*)reftar, &rctar, textures))
        {
            return;
        }

        std::vector<GLVertexInfo> texlist;
        texlist.resize(textures.size() + (method->tar_as_src ? 1 : 0));
        for (unsigned int i = 0; i < textures.size(); ++i)
        {
            tTVPOGLTexture2D* tex = (tTVPOGLTexture2D*)(textures[i].first);
            tex->SyncPixel();
            GLVertexInfo& texitem = texlist[i];
            if (/*_duplicateTargetTexture &&*/ tex == tar)
            {
                tTVPOGLTexture2D* newtex;
                tTVPRect rc = textures[i].second;
                if (reftar)
                {
                    newtex = (tTVPOGLTexture2D*)reftar;
                }
                else if (rc.get_width() >= 0 && rc.get_height() >= 0)
                {
                    newtex = GetTempTexture2D(tex, rc);
                    rc.set_offsets(0, 0);
                }
                else
                {
                    tTVPRect rcsrc(rc);
                    tjs_int w = rc.get_width(), h = rc.get_height();
                    if (w < 0)
                    {
                        std::swap(rcsrc.left, rcsrc.right);
                        rc.right = 0;
                        rc.left = -w;
                    }
                    if (h < 0)
                    {
                        std::swap(rcsrc.top, rcsrc.bottom);
                        rc.bottom = 0;
                        rc.top = -h;
                    }
                    newtex = GetTempTexture2D(tex, rcsrc);
                }
                newtex->ApplyVertex(texitem, rc);
            }
            else
            {
                tex->ApplyVertex(texitem, textures[i].second);
            }
        }
        tar->SyncPixel();
        if (method->tar_as_src)
        {
            tTVPOGLTexture2D* tex = tar;
            GLVertexInfo& texitem = texlist.back();
            //		if (_duplicateTargetTexture)
            {
                tTVPOGLTexture2D* newtex;
                tTVPRect rc = rctar;
                if (reftar)
                {
                    newtex = (tTVPOGLTexture2D*)reftar;
                }
                else
                {
                    newtex = GetTempTexture2D(tex, rc);
                    rc.set_offsets(0, 0);
                }
                newtex->ApplyVertex(texitem, rc);
            }
        }
        static const GLfloat minx = -1, //(GLfloat)rctar.left / tw,
            maxx = 1,                   //(GLfloat)rctar.right / tw,
            miny = -1,                  //(GLfloat)rctar.top / th,
            maxy = 1;                   //(GLfloat)rctar.bottom / th;
        static const GLfloat vertices[12] = {minx, miny, maxx, miny, minx, maxy,

                                             maxx, miny, minx, maxy, maxx, maxy};
        method->Apply();
        tar->AsTarget();
        glViewport(rctar.left * tar->_scaleW, rctar.top * tar->_scaleH,
                   rctar.get_width() * tar->_scaleW, rctar.get_height() * tar->_scaleH);
        int VA_flag = 1 << method->GetPosAttr();
        for (unsigned int i = 0; i < texlist.size(); ++i)
        {
            VA_flag |= 1 << method->GetTexCoordAttr(i);
        }
        _glEnableVertexAttribs(VA_flag);
        glVertexAttribPointer(method->GetPosAttr(), 2, GL_FLOAT, GL_FALSE, 0, vertices);
        for (unsigned int i = 0; i < texlist.size(); ++i)
        {
            method->ApplyTexture(i, texlist[i]);
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
        method->onFinish();
    }

    // src x dst -> tar todo: OperateTriangles
    virtual void OperateTriangles(iTVPRenderMethod* _method,
                                  int nTriangles,
                                  iTVPTexture2D* _tar,
                                  iTVPTexture2D* reftar,
                                  const tTVPRect& rcclip,
                                  const tTVPPointD* _pttar,
                                  const tRenderTexQuadArray& textures) override
    {
        ++_drawCount;
        tTVPOGLRenderMethod* method = (tTVPOGLRenderMethod*)_method;
        tTVPOGLTexture2D* tar = (tTVPOGLTexture2D*)_tar;
        if (_tar == reftar)
            reftar = nullptr;
        int ptcount = nTriangles * 3;

        std::vector<GLVertexInfo> texlist;
        texlist.resize(textures.size() + (method->tar_as_src ? 1 : 0));
        for (unsigned int i = 0; i < textures.size(); ++i)
        {
            tTVPOGLTexture2D* tex = (tTVPOGLTexture2D*)(textures[i].first);
            tex->SyncPixel();
            GLVertexInfo& texitem = texlist[i];
            if (/*_duplicateTargetTexture &&*/ tex == tar)
            {
                tTVPOGLTexture2D* newtex;
                if (reftar)
                    newtex = (tTVPOGLTexture2D*)reftar;
                else
                {
                    newtex =
                        GetTempTexture2D(tex, tTVPRect(0, 0, tex->GetWidth(), tex->GetHeight()));
                }
                newtex->ApplyVertex(texitem, textures[i].second, ptcount);
            }
            else
            {
                tex->ApplyVertex(texitem, textures[i].second, ptcount);
            }
        }
        if (method->tar_as_src)
        {
            tTVPOGLTexture2D* tex = tar;
            GLVertexInfo& texitem = texlist.back();
            {
                if (reftar)
                {
                    static_cast<tTVPOGLTexture2D*>(reftar)->ApplyVertex(texitem, _pttar, ptcount);
                }
                else
                {
                    double l = rcclip.right, t = rcclip.bottom, r = rcclip.left, b = rcclip.top;
                    for (int i = 0; i < ptcount; ++i)
                    {
                        const tTVPPointD& pt = _pttar[i];
                        if (pt.x < l)
                            l = pt.x;
                        if (pt.x > r)
                            r = pt.x;
                        if (pt.y < t)
                            t = pt.y;
                        if (pt.y > b)
                            b = pt.y;
                    }
                    tTVPRect rc(l, t, std::ceil(r), std::ceil(b));
                    if (!TVPIntersectRect(&rc, rcclip, rc))
                    {
                        // nothing to draw
                        return;
                    }
                    l = rc.left;
                    t = rc.top;
                    tTVPOGLTexture2D* newtex = GetTempTexture2D(tex, rc);
                    std::vector<tTVPPointD> pttar;
                    pttar.reserve(ptcount);
                    for (int i = 0; i < ptcount; ++i)
                    {
                        pttar.emplace_back(tTVPPointD{_pttar[i].x - l, _pttar[i].y - t});
                    }
                    newtex->ApplyVertex(texitem, &pttar.front(), ptcount);
                }
            }
        }
        std::vector<GLfloat> pttar;
        float rcw = rcclip.get_width(), rch = rcclip.get_height();
        float rcl = rcclip.left, rct = rcclip.top;
        pttar.resize(ptcount * 2);
        for (int i = 0; i < ptcount; ++i)
        {
            pttar[i * 2 + 0] = (((GLfloat)_pttar[i].x - rcl) / rcw - 0.5f) * 2.f;
            pttar[i * 2 + 1] = (((GLfloat)_pttar[i].y - rct) / rch - 0.5f) * 2.f;
        }
        rcw *= tar->_scaleW;
        rch *= tar->_scaleH;
        rcl *= tar->_scaleW;
        rct *= tar->_scaleH;
        tar->AsTarget();
        glViewport(rcl, rct, rcw, rch);
        method->Apply();
        int VA_flag = 1 << method->GetPosAttr();
        for (unsigned int i = 0; i < texlist.size(); ++i)
        {
            VA_flag |= 1 << method->GetTexCoordAttr(i);
        }
        _glEnableVertexAttribs(VA_flag);
        glVertexAttribPointer(method->GetPosAttr(), 2, GL_FLOAT, GL_FALSE, 0, &pttar.front());

        for (unsigned int i = 0; i < texlist.size(); ++i)
        {
            // tex->SyncPixel();
            method->ApplyTexture(i, texlist[i]);
        }
        glDrawArrays(GL_TRIANGLES, 0, ptcount);
        method->onFinish();
    }

    class tTVPOGLRenderMethod_Perspective : public tTVPOGLRenderMethod_Script
    {
    public:
        int id_Matrix;

        virtual void Rebuild()
        {
            program = CombineProgram(GetVertShader(m_nTex),
                                     CompileShader(GL_FRAGMENT_SHADER, m_strScript));
            glUseProgram(program);
            std::string tex("tex");
            std::string coord("a_texCoord");
            for (int i = 0; i < m_nTex; ++i)
            {
                char sCounter[8];
                sprintf(sCounter, "%d", i);
                int loc = glGetUniformLocation(program, (tex + sCounter).c_str());
                glUniform1i(loc, i);
                loc = glGetAttribLocation(program, (coord + sCounter).c_str());
                tex_coord_attr_location.push_back(loc);
            }
            pos_attr_location = glGetAttribLocation(program, "a_position");
            id_Matrix = EnumParameterID("M");
        }

        virtual void ApplyTexture(unsigned int i, const GLVertexInfo& info)
        {
            info.tex->Bind(i);
        }

        void ApplyMatrix(const float* mtx /*3x3*/)
        {
            glUseProgram(program);
            glUniformMatrix3fv(id_Matrix, 1, GL_FALSE, mtx);
        }
    };

    virtual void OperatePerspective(iTVPRenderMethod* _method,
                                    int nQuads,
                                    iTVPTexture2D* _tar,
                                    iTVPTexture2D* reftar,
                                    const tTVPRect& rcclip,
                                    const tTVPPointD* _pttar /*quad{lt,rt,lb,rb}*/,
                                    const tRenderTexQuadArray& textures) override
    {
        ++_drawCount;
        tTVPOGLTexture2D* tar = (tTVPOGLTexture2D*)_tar;

        tTVPOGLRenderMethod_Perspective* method = (tTVPOGLRenderMethod_Perspective*)_method;

        const tTVPPointD* dstpt = _pttar;
        const tTVPPointD* srcpt = textures[0].second;
        float tw = textures[0].first->GetInternalWidth(),
              th = textures[0].first->GetInternalHeight();
        float dw = _tar->GetInternalWidth(), dh = _tar->GetInternalHeight();

        for (int i = 0; i < nQuads; ++i)
        {
            double srcX[4] = { srcpt[0].x / tw, (srcpt[1].x + 1) / tw, (srcpt[3].x + 1) / tw, srcpt[2].x / tw };
            double srcY[4] = { srcpt[0].y / th, srcpt[1].y / th, (srcpt[3].y + 1) / th, (srcpt[2].y + 1) / th };
            double dstX[4] = { dstpt[0].x * tar->_scaleW, dstpt[1].x * tar->_scaleW, dstpt[3].x * tar->_scaleW, dstpt[2].x * tar->_scaleW };
            double dstY[4] = { dstpt[0].y * tar->_scaleH, dstpt[1].y * tar->_scaleH, dstpt[3].y * tar->_scaleH, dstpt[2].y * tar->_scaleH };

            double perspMat[8];
            // Get dst->src mapping (swap src/dst arguments)
            TVPImageUtils::GetPerspectiveTransform(dstX, dstY, srcX, srcY, perspMat);
            float mtx[9] = {
                (float)perspMat[0], (float)perspMat[1], (float)perspMat[2],
                (float)perspMat[3], (float)perspMat[4], (float)perspMat[5],
                (float)perspMat[6], (float)perspMat[7], 1.0f
            };
            method->ApplyMatrix(mtx);

            // pass to OperateTriangles
            tTVPPointD pttar[6] =
                {
                    dstpt[0], dstpt[1], dstpt[2],

                    dstpt[1], dstpt[2], dstpt[3],
                },
                       pttex[6] = {srcpt[0], srcpt[1], srcpt[2],

                                   srcpt[1], srcpt[2], srcpt[3]};

            *(tTVPPointD**)&textures[0].second = &pttex[0];

            dstpt += 4;
            srcpt += 4;

            OperateTriangles(method, nQuads * 2, _tar, reftar, rcclip, &pttar[0], textures);
        }
    }

    virtual void BeginStencil(iTVPTexture2D* reftex) override
    {
        if (_CurrentFBOValid && _CurrentRenderTarget)
        {
            glGetIntegerv(GL_RENDERBUFFER_BINDING, &_prevRenderBuffer);
            glBindRenderbuffer(GL_RENDERBUFFER, _stencil_FBO);
            bool recreateRenderBuffer = false;
            unsigned int w = reftex->GetInternalWidth(), h = reftex->GetInternalHeight();
            if (_stencilBufferW != w)
            {
                _stencilBufferW = w;
                recreateRenderBuffer = true;
            }
            if (_stencilBufferH != h)
            {
                _stencilBufferH = h;
                recreateRenderBuffer = true;
            }
            if (recreateRenderBuffer)
            {
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, _stencilBufferW,
                                      _stencilBufferH);
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                      _stencil_FBO);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                      _stencil_FBO);
        }
    }

    virtual void EndStencil() override
    {
        glDisable(GL_STENCIL_TEST);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, _prevRenderBuffer);
    }

    virtual void SetRenderTarget(iTVPTexture2D* target) override
    {
        static_cast<tTVPOGLTexture2D*>(target)->AsTarget();
    }
};

static iTVPRenderManager* __TVPRenderManager_OpenGLFactory()
{
    return new TVPRenderManager_OpenGL();
}
static class __TVPRenderManager_OpenGLAutoReigster
{
public:
    __TVPRenderManager_OpenGLAutoReigster()
    {
        TVPRegisterRenderManager("opengl", __TVPRenderManager_OpenGLFactory);
    }
} __TVPRenderManager_OpenGLAutoReigster_instance;