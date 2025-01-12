/*
 * Copyright 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SOFT_VIDEO_ENCODER_OMX_COMPONENT_H_

#define SOFT_VIDEO_ENCODER_OMX_COMPONENT_H_

#include <media/openmax/OMX_Core.h>
#include <media/openmax/OMX_Video.h>
#include <media/openmax/OMX_VideoExt.h>

#include "SimpleSoftOMXComponent.h"

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/eglplatform.h>

struct hw_module_t;

namespace android {

struct SoftVideoEncoderOMXComponent : public SimpleSoftOMXComponent {
    SoftVideoEncoderOMXComponent(
            const char *name,
            const char *componentRole,
            OMX_VIDEO_CODINGTYPE codingType,
            const CodecProfileLevel *profileLevels,
            size_t numProfileLevels,
            int32_t width,
            int32_t height,
            const OMX_CALLBACKTYPE *callbacks,
            OMX_PTR appData,
            OMX_COMPONENTTYPE **component);

    virtual OMX_ERRORTYPE internalSetParameter(OMX_INDEXTYPE index, const OMX_PTR param);
    virtual OMX_ERRORTYPE internalGetParameter(OMX_INDEXTYPE index, OMX_PTR params);

protected:
    void initPorts(
            OMX_U32 numInputBuffers, OMX_U32 numOutputBuffers, OMX_U32 outputBufferSize,
            const char *mime, OMX_U32 minCompressionRatio = 1);
    void initEgl(size_t width, size_t height, const uint8_t *src);
    void closeEgl();

    static void setRawVideoSize(OMX_PARAM_PORTDEFINITIONTYPE *def);

    static void ConvertFlexYUVToPlanar(
            uint8_t *dst, size_t dstStride, size_t dstVStride,
            struct android_ycbcr *ycbcr, int32_t width, int32_t height);

    static void ConvertYUV420SemiPlanarToYUV420Planar(
            const uint8_t *inYVU, uint8_t* outYUV, int32_t width, int32_t height);

    static void ConvertRGB32ToPlanar(
        uint8_t *dstY, size_t dstStride, size_t dstVStride,
        const uint8_t *src, size_t width, size_t height, size_t srcStride,
        bool bgr);

    const uint8_t *extractGraphicBuffer(
            uint8_t *dst, size_t dstSize, const uint8_t *src, size_t srcSize,
            size_t width, size_t height) const;

    virtual OMX_ERRORTYPE getExtensionIndex(const char *name, OMX_INDEXTYPE *index);

    OMX_ERRORTYPE validateInputBuffer(const OMX_BUFFERHEADERTYPE *inputBufferHeader);

    void drawQuad(int x, int y, int w, int h) const;
    GLint createProgram(const char* vs, const char* fs);
    const char *eglStrError(EGLint err);

    enum {
        kInputPortIndex = 0,
        kOutputPortIndex = 1,
    };

    bool mInputDataIsMeta;
    int32_t mWidth;      // width of the input frames
    int32_t mHeight;     // height of the input frames
    uint32_t mBitrate;   // target bitrate set for the encoder, in bits per second
    uint32_t mFramerate; // target framerate set for the encoder, in Q16 format
    OMX_COLOR_FORMATTYPE mColorFormat;  // Color format for the input port

private:
    void updatePortParams();
    OMX_ERRORTYPE internalSetPortParams(const OMX_PARAM_PORTDEFINITIONTYPE* port);

    static const uint32_t kInputBufferAlignment = 1;
    static const uint32_t kOutputBufferAlignment = 2;

    mutable const hw_module_t *mGrallocModule;

    uint32_t mMinOutputBufferSize;
    uint32_t mMinCompressionRatio;

    const char *mComponentRole;
    OMX_VIDEO_CODINGTYPE mCodingType;
    const CodecProfileLevel *mProfileLevels;
    size_t mNumProfileLevels;

    bool mIsPowervr = false;
    EGLDisplay mEglDisplay = EGL_NO_DISPLAY;
    EGLContext mEglContext = EGL_NO_CONTEXT;
    EGLSurface mEglSurface = EGL_NO_SURFACE;

    const char *vertSource =
        "precision mediump float;\n"
        "attribute vec2 in_position;\n"
        "attribute vec2 in_texcoord;\n"
        "varying vec2 texcoord;\n"
        "\n"
        "void main()\n"
        "{\n"
        "   gl_Position = vec4(in_position, 0.0, 1.0);\n"
        "   texcoord = in_texcoord;\n"
        "}\n";

    const char *fragSource =
        "precision mediump float;\n"
        "varying vec2 texcoord;\n"
        "uniform sampler2D texture;\n"
        "\n"
        "void main()\n"
        "{\n"
        "   gl_FragColor = texture2D(texture, texcoord);\n"
        "}\n";

    const char *vertSourceYuv = "attribute vec4 vPosition;\n"
        "attribute vec2 vYuvTexCoords;\n"
        "varying vec2 yuvTexCoords;\n"
        "void main() {\n"
        "  yuvTexCoords = vYuvTexCoords;\n"
        "  gl_Position = vPosition;\n"
        "}\n";

    const char *fragSourceYuv = "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "uniform samplerExternalOES yuvTexSampler;\n"
        "varying vec2 yuvTexCoords;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(yuvTexSampler, yuvTexCoords);\n"
        "}\n";
    GLint mPosition;
    GLint mYuvPosition;
    GLint mYuvTexSampler;
    GLubyte *mShmData = NULL;
    GLint mProgram = 0;

    DISALLOW_EVIL_CONSTRUCTORS(SoftVideoEncoderOMXComponent);
};

}  // namespace android

#endif  // SOFT_VIDEO_ENCODER_OMX_COMPONENT_H_
