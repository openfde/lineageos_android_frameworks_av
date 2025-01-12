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

#include <inttypes.h>

#define LOG_NDEBUG 0
#define LOG_TAG "SoftVideoEncoderOMXComponent"
#include <cutils/properties.h>
#include <utils/Log.h>
#include <utils/misc.h>

#include <media/stagefright/omx/SoftVideoEncoderOMXComponent.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/AUtils.h>
#include <media/stagefright/foundation/MediaDefs.h>
#include <media/hardware/HardwareAPI.h>
#include <media/openmax/OMX_IndexExt.h>

#include <ui/Fence.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>

#include <hardware/gralloc.h>
#include <nativebase/nativebase.h>

namespace android {

const static OMX_COLOR_FORMATTYPE kSupportedColorFormats[] = {
    OMX_COLOR_FormatYUV420Planar,
    OMX_COLOR_FormatYUV420SemiPlanar,
    OMX_COLOR_FormatAndroidOpaque
};

const GLfloat kPositionVertices[] = {
	-1.0f, 1.0f,
	-1.0f, -1.0f,
	1.0f, -1.0f,
	1.0f, 1.0f,
};

const GLfloat kYuvPositionVertices[] = {
	0.0f, 1.0f,
	0.0f, 0.0f,
	1.0f, 0.0f,
	1.0f, 1.0f,
};

template<class T>
static void InitOMXParams(T *params) {
    params->nSize = sizeof(T);
    params->nVersion.s.nVersionMajor = 1;
    params->nVersion.s.nVersionMinor = 0;
    params->nVersion.s.nRevision = 0;
    params->nVersion.s.nStep = 0;
}

SoftVideoEncoderOMXComponent::SoftVideoEncoderOMXComponent(
        const char *name,
        const char *componentRole,
        OMX_VIDEO_CODINGTYPE codingType,
        const CodecProfileLevel *profileLevels,
        size_t numProfileLevels,
        int32_t width,
        int32_t height,
        const OMX_CALLBACKTYPE *callbacks,
        OMX_PTR appData,
        OMX_COMPONENTTYPE **component)
    : SimpleSoftOMXComponent(name, callbacks, appData, component),
      mInputDataIsMeta(false),
      mWidth(width),
      mHeight(height),
      mBitrate(192000),
      mFramerate(30 << 16), // Q16 format
      mColorFormat(OMX_COLOR_FormatYUV420Planar),
      mMinOutputBufferSize(384), // arbitrary, using one uncompressed macroblock
      mMinCompressionRatio(1),   // max output size is normally the input size
      mComponentRole(componentRole),
      mCodingType(codingType),
      mProfileLevels(profileLevels),
      mNumProfileLevels(numProfileLevels) {
    char property[PROPERTY_VALUE_MAX];
    if (property_get("ro.hardware.egl", property, "default") > 0){
        if (strcmp(property, "powervr") == 0)
            mIsPowervr = true;
    }
}

void SoftVideoEncoderOMXComponent::initPorts(
        OMX_U32 numInputBuffers, OMX_U32 numOutputBuffers, OMX_U32 outputBufferSize,
        const char *mime, OMX_U32 minCompressionRatio) {
    OMX_PARAM_PORTDEFINITIONTYPE def;

    mMinOutputBufferSize = outputBufferSize;
    mMinCompressionRatio = minCompressionRatio;

    InitOMXParams(&def);

    def.nPortIndex = kInputPortIndex;
    def.eDir = OMX_DirInput;
    def.nBufferCountMin = numInputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mWidth;
    def.format.video.nFrameHeight = mHeight;
    def.format.video.nStride = def.format.video.nFrameWidth;
    def.format.video.nSliceHeight = def.format.video.nFrameHeight;
    def.format.video.nBitrate = 0;
    // frameRate is in Q16 format.
    def.format.video.xFramerate = mFramerate;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.nBufferAlignment = kInputBufferAlignment;
    def.format.video.cMIMEType = const_cast<char *>("video/raw");
    def.format.video.eCompressionFormat = OMX_VIDEO_CodingUnused;
    def.format.video.eColorFormat = mColorFormat;
    def.format.video.pNativeWindow = NULL;
    // buffersize set in updatePortParams

    addPort(def);

    InitOMXParams(&def);

    def.nPortIndex = kOutputPortIndex;
    def.eDir = OMX_DirOutput;
    def.nBufferCountMin = numOutputBuffers;
    def.nBufferCountActual = def.nBufferCountMin;
    def.bEnabled = OMX_TRUE;
    def.bPopulated = OMX_FALSE;
    def.eDomain = OMX_PortDomainVideo;
    def.bBuffersContiguous = OMX_FALSE;
    def.format.video.pNativeRender = NULL;
    def.format.video.nFrameWidth = mWidth;
    def.format.video.nFrameHeight = mHeight;
    def.format.video.nStride = 0;
    def.format.video.nSliceHeight = 0;
    def.format.video.nBitrate = mBitrate;
    def.format.video.xFramerate = 0 << 16;
    def.format.video.bFlagErrorConcealment = OMX_FALSE;
    def.nBufferAlignment = kOutputBufferAlignment;
    def.format.video.cMIMEType = const_cast<char *>(mime);
    def.format.video.eCompressionFormat = mCodingType;
    def.format.video.eColorFormat = OMX_COLOR_FormatUnused;
    def.format.video.pNativeWindow = NULL;
    // buffersize set in updatePortParams

    addPort(def);

    updatePortParams();
}

void SoftVideoEncoderOMXComponent::initEgl(size_t width, size_t height, const uint8_t *src){
    if (mIsPowervr) {
        if(mEglDisplay == EGL_NO_DISPLAY){
            ALOGE("initEgl width: %zu, height: %zu", width, height);
            MetadataBufferType bufferType = *(MetadataBufferType *)src;
            bool usingANWBuffer = bufferType == kMetadataBufferTypeANWBuffer;
            if (!usingANWBuffer && bufferType != kMetadataBufferTypeGrallocSource) {
                ALOGE("Unsupported metadata type (%d)", bufferType);
                return ;
            }
            int format;
            if (usingANWBuffer) {
                VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)src;
                ANativeWindowBuffer_t *buffer = nativeMeta.pBuffer;
                format = buffer->format;
            } else {
                format = HAL_PIXEL_FORMAT_RGBA_8888;
            }
            bool isYuv = (format == HAL_PIXEL_FORMAT_RGBX_8888 || format == HAL_PIXEL_FORMAT_RGBA_8888 || format == HAL_PIXEL_FORMAT_BGRA_8888) ? false : true;
            mEglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            eglInitialize(mEglDisplay, NULL, NULL);
            ALOGI("eglInitialize: %s", eglStrError(eglGetError()));

            EGLConfig config;
            int num_config;
            EGLint dpy_attrs[] = {
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_NONE };
            eglChooseConfig(mEglDisplay, dpy_attrs, &config, 1, &num_config);
            ALOGI("eglChooseConfig: %s", eglStrError(eglGetError()));

            EGLint context_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
            mEglContext = eglCreateContext(mEglDisplay, config,  EGL_NO_CONTEXT, context_attrs);
            ALOGI("eglCreateContext: %s", eglStrError(eglGetError()));

            EGLint pbuf_attrs[] = { EGL_WIDTH, (EGLint)width, EGL_HEIGHT, (EGLint)height, EGL_NONE };
            mEglSurface = eglCreatePbufferSurface(mEglDisplay, config, pbuf_attrs);
            ALOGI("eglCreatePbufferSurface: %s", eglStrError(eglGetError()));

            eglMakeCurrent(mEglDisplay, mEglSurface, mEglSurface, mEglContext);
            ALOGI("eglMakeCurrent: %s", eglStrError(eglGetError()));
            mProgram = createProgram(isYuv ? vertSourceYuv : vertSource, isYuv ? fragSourceYuv : fragSource);
            glUseProgram(mProgram);
            ALOGI("glUseProgram: %d", glGetError());
            if (isYuv) {
                mPosition = glGetAttribLocation(mProgram, "vPosition");
                ALOGI("glGetAttribLocation: %s", eglStrError(eglGetError()));
                mYuvPosition = glGetAttribLocation(mProgram, "vYuvTexCoords");
                ALOGI("glGetAttribLocation: %s", eglStrError(eglGetError()));
                mYuvTexSampler = glGetUniformLocation(mProgram, "yuvTexSampler");
                ALOGI("glGetUniformLocation: %s", eglStrError(eglGetError()));
                glVertexAttribPointer(mPosition, 2, GL_FLOAT, GL_FALSE, 0, kPositionVertices);
                ALOGI("glVertexAttribPointer: %d", glGetError());
                glEnableVertexAttribArray(mPosition);
                ALOGI("glEnableVertexAttribArray: %d", glGetError());
                glVertexAttribPointer(mYuvPosition, 2, GL_FLOAT, GL_FALSE, 0, kYuvPositionVertices);
                ALOGI("glVertexAttribPointer: %d", glGetError());
                glEnableVertexAttribArray(mYuvPosition);
                ALOGI("glEnableVertexAttribArray: %d", glGetError());
                glUniform1i(mYuvTexSampler, 0);
                ALOGI("glUniform1i: %d", glGetError());
                glViewport(0, 0, width, height);
                ALOGI("glViewport: %s", eglStrError(eglGetError()));
            }
        }
    }
}

GLint SoftVideoEncoderOMXComponent::createProgram(const char* vs, const char* fs) {
    GLint success = 0;
    GLint logLength = 0;
    char infoLog[1024];

    GLint vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vs, 0);
    glCompileShader(vertexShader);
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(vertexShader, sizeof(infoLog), &logLength, infoLog);
        ALOGE("Vertex shader compilation failed:\n%s\n", infoLog);
    }

    GLint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fs, 0);
    glCompileShader(fragmentShader);
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        glGetShaderInfoLog(fragmentShader, sizeof(infoLog), &logLength, infoLog);
        ALOGE("Fragment shader compilation failed:\n%s\n", infoLog);
    }

    GLint program = glCreateProgram();
    glAttachShader(program, fragmentShader);
    glAttachShader(program, vertexShader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        glGetProgramInfoLog(program, sizeof(infoLog), &logLength, infoLog);
        ALOGE("Program linking failed:\n%s\n", infoLog);
    }

    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    return program;
}

const char *SoftVideoEncoderOMXComponent::eglStrError(EGLint err){
    switch (err){
        case EGL_SUCCESS:           return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED:   return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS:        return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC:         return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE:     return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG:        return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT:       return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY:       return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH:         return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER:     return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE:       return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST:      return "EGL_CONTEXT_LOST";
        default: return "UNKNOWN";
    }
}

void SoftVideoEncoderOMXComponent::closeEgl(){
    ALOGE("closeEgl isPowervr: %x", mIsPowervr);
    if (mIsPowervr) {
        if(mShmData != NULL){
            delete[] mShmData;
            mShmData = NULL;
            ALOGE("mShmData deleted and seted to NULL");
        }
        if (mEglDisplay == EGL_NO_DISPLAY){
           return;
        }
        if(mProgram != 0){
            glDeleteProgram(mProgram);
            mProgram = 0;
        }

        eglMakeCurrent(mEglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

        if (mEglSurface != EGL_NO_SURFACE)
            eglDestroySurface(mEglDisplay, mEglSurface);
        if (mEglContext != EGL_NO_CONTEXT)
            eglDestroyContext(mEglDisplay, mEglContext);

        eglTerminate(mEglDisplay);

        mEglDisplay = EGL_NO_DISPLAY;
        mEglSurface = EGL_NO_SURFACE;
        mEglContext = EGL_NO_CONTEXT;
    }
}

void SoftVideoEncoderOMXComponent::updatePortParams() {
    OMX_PARAM_PORTDEFINITIONTYPE *inDef = &editPortInfo(kInputPortIndex)->mDef;
    inDef->format.video.nFrameWidth = mWidth;
    inDef->format.video.nFrameHeight = mHeight;
    inDef->format.video.nStride = inDef->format.video.nFrameWidth;
    inDef->format.video.nSliceHeight = inDef->format.video.nFrameHeight;
    inDef->format.video.xFramerate = mFramerate;
    inDef->format.video.eColorFormat = mColorFormat;
    uint32_t rawBufferSize =
        inDef->format.video.nStride * inDef->format.video.nSliceHeight * 3 / 2;
    if (inDef->format.video.eColorFormat == OMX_COLOR_FormatAndroidOpaque) {
        inDef->nBufferSize = max(sizeof(VideoNativeMetadata), sizeof(VideoGrallocMetadata));
    } else {
        inDef->nBufferSize = rawBufferSize;
    }

    OMX_PARAM_PORTDEFINITIONTYPE *outDef = &editPortInfo(kOutputPortIndex)->mDef;
    outDef->format.video.nFrameWidth = mWidth;
    outDef->format.video.nFrameHeight = mHeight;
    outDef->format.video.nBitrate = mBitrate;

    outDef->nBufferSize = max(mMinOutputBufferSize, rawBufferSize / mMinCompressionRatio);
}

OMX_ERRORTYPE SoftVideoEncoderOMXComponent::internalSetPortParams(
        const OMX_PARAM_PORTDEFINITIONTYPE *port) {

    if (!isValidOMXParam(port)) {
        return OMX_ErrorBadParameter;
    }

    if (port->nPortIndex == kInputPortIndex) {
        mWidth = port->format.video.nFrameWidth;
        mHeight = port->format.video.nFrameHeight;
        if(mIsPowervr){
            if(mShmData == NULL){
                mShmData = new GLubyte[mWidth * mHeight * 4];
                ALOGE("mShmData seted size: %d", mWidth * mHeight * 4);
            }
        }

        // xFramerate comes in Q16 format, in frames per second unit
        mFramerate = port->format.video.xFramerate;

        if (port->format.video.eCompressionFormat != OMX_VIDEO_CodingUnused
                || (port->format.video.eColorFormat != OMX_COLOR_FormatYUV420Planar
                        && port->format.video.eColorFormat != OMX_COLOR_FormatYUV420SemiPlanar
                        && port->format.video.eColorFormat != OMX_COLOR_FormatAndroidOpaque)) {
            return OMX_ErrorUnsupportedSetting;
        }

        mColorFormat = port->format.video.eColorFormat;
    } else if (port->nPortIndex == kOutputPortIndex) {
        if (port->format.video.eCompressionFormat != mCodingType
                || port->format.video.eColorFormat != OMX_COLOR_FormatUnused) {
            return OMX_ErrorUnsupportedSetting;
        }

        mBitrate = port->format.video.nBitrate;
    } else {
        return OMX_ErrorBadPortIndex;
    }

    updatePortParams();
    return OMX_ErrorNone;
}

OMX_ERRORTYPE SoftVideoEncoderOMXComponent::internalSetParameter(
        OMX_INDEXTYPE index, const OMX_PTR param) {
    // can include extension index OMX_INDEXEXTTYPE
    const int32_t indexFull = index;

    switch (indexFull) {
        case OMX_IndexParamVideoErrorCorrection:
        {
            return OMX_ErrorNotImplemented;
        }

        case OMX_IndexParamStandardComponentRole:
        {
            const OMX_PARAM_COMPONENTROLETYPE *roleParams =
                (const OMX_PARAM_COMPONENTROLETYPE *)param;

            if (!isValidOMXParam(roleParams)) {
                return OMX_ErrorBadParameter;
            }

            if (strncmp((const char *)roleParams->cRole,
                        mComponentRole,
                        OMX_MAX_STRINGNAME_SIZE - 1)) {
                return OMX_ErrorUnsupportedSetting;
            }

            return OMX_ErrorNone;
        }

        case OMX_IndexParamPortDefinition:
        {
            OMX_ERRORTYPE err = internalSetPortParams((const OMX_PARAM_PORTDEFINITIONTYPE *)param);

            if (err != OMX_ErrorNone) {
                return err;
            }

            return SimpleSoftOMXComponent::internalSetParameter(index, param);
        }

        case OMX_IndexParamVideoPortFormat:
        {
            const OMX_VIDEO_PARAM_PORTFORMATTYPE* format =
                (const OMX_VIDEO_PARAM_PORTFORMATTYPE *)param;

            if (!isValidOMXParam(format)) {
                return OMX_ErrorBadParameter;
            }

            if (format->nPortIndex == kInputPortIndex) {
                if (format->eColorFormat == OMX_COLOR_FormatYUV420Planar ||
                    format->eColorFormat == OMX_COLOR_FormatYUV420SemiPlanar ||
                    format->eColorFormat == OMX_COLOR_FormatAndroidOpaque) {
                    mColorFormat = format->eColorFormat;

                    updatePortParams();
                    return OMX_ErrorNone;
                } else {
                    ALOGE("Unsupported color format %i", format->eColorFormat);
                    return OMX_ErrorUnsupportedSetting;
                }
            } else if (format->nPortIndex == kOutputPortIndex) {
                if (format->eCompressionFormat == mCodingType) {
                    return OMX_ErrorNone;
                } else {
                    return OMX_ErrorUnsupportedSetting;
                }
            } else {
                return OMX_ErrorBadPortIndex;
            }
        }

        case kStoreMetaDataExtensionIndex:
        {
            // storeMetaDataInBuffers
            const StoreMetaDataInBuffersParams *storeParam =
                (const StoreMetaDataInBuffersParams *)param;

            if (!isValidOMXParam(storeParam)) {
                return OMX_ErrorBadParameter;
            }

            if (storeParam->nPortIndex == kOutputPortIndex) {
                return storeParam->bStoreMetaData ? OMX_ErrorUnsupportedSetting : OMX_ErrorNone;
            } else if (storeParam->nPortIndex != kInputPortIndex) {
                return OMX_ErrorBadPortIndex;
            }

            mInputDataIsMeta = (storeParam->bStoreMetaData == OMX_TRUE);
            if (mInputDataIsMeta) {
                mColorFormat = OMX_COLOR_FormatAndroidOpaque;
            } else if (mColorFormat == OMX_COLOR_FormatAndroidOpaque) {
                mColorFormat = OMX_COLOR_FormatYUV420Planar;
            }
            updatePortParams();
            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalSetParameter(index, param);
    }
}

OMX_ERRORTYPE SoftVideoEncoderOMXComponent::internalGetParameter(
        OMX_INDEXTYPE index, OMX_PTR param) {
    switch ((int)index) {
        case OMX_IndexParamVideoErrorCorrection:
        {
            return OMX_ErrorNotImplemented;
        }

        case OMX_IndexParamVideoPortFormat:
        {
            OMX_VIDEO_PARAM_PORTFORMATTYPE *formatParams =
                (OMX_VIDEO_PARAM_PORTFORMATTYPE *)param;

            if (!isValidOMXParam(formatParams)) {
                return OMX_ErrorBadParameter;
            }

            if (formatParams->nPortIndex == kInputPortIndex) {
                if (formatParams->nIndex >= NELEM(kSupportedColorFormats)) {
                    return OMX_ErrorNoMore;
                }

                // Color formats, in order of preference
                formatParams->eColorFormat = kSupportedColorFormats[formatParams->nIndex];
                formatParams->eCompressionFormat = OMX_VIDEO_CodingUnused;
                formatParams->xFramerate = mFramerate;
                return OMX_ErrorNone;
            } else if (formatParams->nPortIndex == kOutputPortIndex) {
                formatParams->eCompressionFormat = mCodingType;
                formatParams->eColorFormat = OMX_COLOR_FormatUnused;
                formatParams->xFramerate = 0;
                return OMX_ErrorNone;
            } else {
                return OMX_ErrorBadPortIndex;
            }
        }

        case OMX_IndexParamVideoProfileLevelQuerySupported:
        {
            OMX_VIDEO_PARAM_PROFILELEVELTYPE *profileLevel =
                  (OMX_VIDEO_PARAM_PROFILELEVELTYPE *) param;

            if (!isValidOMXParam(profileLevel)) {
                return OMX_ErrorBadParameter;
            }

            if (profileLevel->nPortIndex != kOutputPortIndex) {
                ALOGE("Invalid port index: %u", profileLevel->nPortIndex);
                return OMX_ErrorUnsupportedIndex;
            }

            if (profileLevel->nProfileIndex >= mNumProfileLevels) {
                return OMX_ErrorNoMore;
            }

            profileLevel->eProfile = mProfileLevels[profileLevel->nProfileIndex].mProfile;
            profileLevel->eLevel   = mProfileLevels[profileLevel->nProfileIndex].mLevel;
            return OMX_ErrorNone;
        }

        case OMX_IndexParamConsumerUsageBits:
        {
            OMX_U32 *usageBits = (OMX_U32 *)param;
            *usageBits = GRALLOC_USAGE_SW_READ_OFTEN;
            return OMX_ErrorNone;
        }

        case OMX_IndexParamPortDefinition:
        {
            OMX_PARAM_PORTDEFINITIONTYPE *def =
                (OMX_PARAM_PORTDEFINITIONTYPE *)param;

            if (def->nPortIndex > 1) {
                return OMX_ErrorUndefined;
            }

            OMX_ERRORTYPE err = SimpleSoftOMXComponent::internalGetParameter(index, param);
            if (OMX_ErrorNone != err) {
                return err;
            }

            def->format.video.nFrameWidth = mWidth;
            def->format.video.nFrameHeight = mHeight;

            // XXX: For now just configure input and output buffers the same size.
            // May want to determine a more suitable output buffer size independent
            // of YUV format.
            char property[PROPERTY_VALUE_MAX];
            bool isMesa = false;
            bool isPowervr = false;
            bool isEmulation = false;
            if (property_get("ro.hardware.egl", property, "default") > 0){
                if (strcmp(property, "mesa") == 0)
                    isMesa = true;
                if (strcmp(property, "powervr") == 0)
                    isPowervr = true;
                if (strcmp(property, "emulation") == 0)
                    isEmulation = true;
            }
            ALOGE("fde mColorFormat: %x", mColorFormat);
            if (!isMesa && !isPowervr && !isEmulation) {
                CHECK(mColorFormat == OMX_COLOR_FormatYUV420Planar ||
                        mColorFormat == OMX_COLOR_FormatYUV420SemiPlanar);
            }
            def->nBufferSize = mWidth * mHeight * 3 / 2;

            return OMX_ErrorNone;
        }

        default:
            return SimpleSoftOMXComponent::internalGetParameter(index, param);
    }
}

// static
__attribute__((no_sanitize("integer")))
void SoftVideoEncoderOMXComponent::ConvertFlexYUVToPlanar(
        uint8_t *dst, size_t dstStride, size_t dstVStride,
        struct android_ycbcr *ycbcr, int32_t width, int32_t height) {
    const uint8_t *src = (const uint8_t *)ycbcr->y;
    const uint8_t *srcU = (const uint8_t *)ycbcr->cb;
    const uint8_t *srcV = (const uint8_t *)ycbcr->cr;
    uint8_t *dstU = dst + dstVStride * dstStride;
    uint8_t *dstV = dstU + (dstVStride >> 1) * (dstStride >> 1);

    for (size_t y = height; y > 0; --y) {
        memcpy(dst, src, width);
        dst += dstStride;
        src += ycbcr->ystride;
    }
    if (ycbcr->cstride == ycbcr->ystride >> 1 && ycbcr->chroma_step == 1) {
        // planar
        for (size_t y = height >> 1; y > 0; --y) {
            memcpy(dstU, srcU, width >> 1);
            dstU += dstStride >> 1;
            srcU += ycbcr->cstride;
            memcpy(dstV, srcV, width >> 1);
            dstV += dstStride >> 1;
            srcV += ycbcr->cstride;
        }
    } else {
        // arbitrary
        for (size_t y = height >> 1; y > 0; --y) {
            for (size_t x = width >> 1; x > 0; --x) {
                *dstU++ = *srcU;
                *dstV++ = *srcV;
                srcU += ycbcr->chroma_step;
                srcV += ycbcr->chroma_step;
            }
            dstU += (dstStride >> 1) - (width >> 1);
            dstV += (dstStride >> 1) - (width >> 1);
            srcU += ycbcr->cstride - (width >> 1) * ycbcr->chroma_step;
            srcV += ycbcr->cstride - (width >> 1) * ycbcr->chroma_step;
        }
    }
}

// static
__attribute__((no_sanitize("integer")))
void SoftVideoEncoderOMXComponent::ConvertYUV420SemiPlanarToYUV420Planar(
        const uint8_t *inYVU, uint8_t* outYUV, int32_t width, int32_t height) {
    // TODO: add support for stride
    int32_t outYsize = width * height;
    uint32_t *outY  = (uint32_t *) outYUV;
    uint16_t *outCb = (uint16_t *) (outYUV + outYsize);
    uint16_t *outCr = (uint16_t *) (outYUV + outYsize + (outYsize >> 2));

    /* Y copying */
    memcpy(outY, inYVU, outYsize);

    /* U & V copying */
    // FIXME this only works if width is multiple of 4
    uint32_t *inYVU_4 = (uint32_t *) (inYVU + outYsize);
    for (int32_t i = height >> 1; i > 0; --i) {
        for (int32_t j = width >> 2; j > 0; --j) {
            uint32_t temp = *inYVU_4++;
            uint32_t tempU = temp & 0xFF;
            tempU = tempU | ((temp >> 8) & 0xFF00);

            uint32_t tempV = (temp >> 8) & 0xFF;
            tempV = tempV | ((temp >> 16) & 0xFF00);

            *outCb++ = tempU;
            *outCr++ = tempV;
        }
    }
}

// static
__attribute__((no_sanitize("integer")))
void SoftVideoEncoderOMXComponent::ConvertRGB32ToPlanar(
        uint8_t *dstY, size_t dstStride, size_t dstVStride,
        const uint8_t *src, size_t width, size_t height, size_t srcStride,
        bool bgr) {
    CHECK((width & 1) == 0);
    CHECK((height & 1) == 0);

    uint8_t *dstU = dstY + dstStride * dstVStride;
    uint8_t *dstV = dstU + (dstStride >> 1) * (dstVStride >> 1);

#ifdef SURFACE_IS_BGR32
    bgr = !bgr;
#endif

    const size_t redOffset   = bgr ? 2 : 0;
    const size_t greenOffset = 1;
    const size_t blueOffset  = bgr ? 0 : 2;

    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            unsigned red   = src[redOffset];
            unsigned green = src[greenOffset];
            unsigned blue  = src[blueOffset];

            // Using ITU-R BT.601-7 (03/2011)
            //   2.5.1: Ey'  = ( 0.299*R + 0.587*G + 0.114*B)
            //   2.5.2: ECr' = ( 0.701*R - 0.587*G - 0.114*B) / 1.402
            //          ECb' = (-0.299*R - 0.587*G + 0.886*B) / 1.772
            //   2.5.3: Y  = 219 * Ey'  +  16
            //          Cr = 224 * ECr' + 128
            //          Cb = 224 * ECb' + 128

            unsigned luma =
                ((red * 65 + green * 129 + blue * 25 + 128) >> 8) + 16;

            dstY[x] = luma;

            if ((x & 1) == 0 && (y & 1) == 0) {
                unsigned U =
                    ((-red * 38 - green * 74 + blue * 112 + 128) >> 8) + 128;

                unsigned V =
                    ((red * 112 - green * 94 - blue * 18 + 128) >> 8) + 128;

                dstU[x >> 1] = U;
                dstV[x >> 1] = V;
            }
            src += 4;
        }

        if ((y & 1) == 0) {
            dstU += dstStride >> 1;
            dstV += dstStride >> 1;
        }

        src += srcStride - 4 * width;
        dstY += dstStride;
    }
}

const uint8_t *SoftVideoEncoderOMXComponent::extractGraphicBuffer(
        uint8_t *dst, size_t dstSize,
        const uint8_t *src, size_t srcSize,
        size_t width, size_t height) const {
    size_t dstStride = width;
    size_t dstVStride = height;

    MetadataBufferType bufferType = *(MetadataBufferType *)src;
    bool usingANWBuffer = bufferType == kMetadataBufferTypeANWBuffer;
    if (!usingANWBuffer && bufferType != kMetadataBufferTypeGrallocSource) {
        ALOGE("Unsupported metadata type (%d)", bufferType);
        return NULL;
    }

    buffer_handle_t handle;
    int format;
    size_t srcStride;
    size_t srcVStride;
    if (usingANWBuffer) {
        if (srcSize < sizeof(VideoNativeMetadata)) {
            ALOGE("Metadata is too small (%zu vs %zu)", srcSize, sizeof(VideoNativeMetadata));
            return NULL;
        }

        VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)src;
        ANativeWindowBuffer *buffer = nativeMeta.pBuffer;
        handle = buffer->handle;
        format = buffer->format;
        srcStride = buffer->stride;
        srcVStride = buffer->height;
        // convert stride from pixels to bytes
        if (mIsPowervr || (format != HAL_PIXEL_FORMAT_YV12 &&
            format != HAL_PIXEL_FORMAT_YCrCb_420_SP &&
            format != HAL_PIXEL_FORMAT_YCbCr_420_888)) {
            // TODO do we need to support other formats?
            srcStride *= 4;
        }

        if (nativeMeta.nFenceFd >= 0) {
            sp<Fence> fence = new Fence(nativeMeta.nFenceFd);
            nativeMeta.nFenceFd = -1;
            status_t err = fence->wait(kFenceTimeoutMs);
            if (err != OK) {
                ALOGE("Timed out waiting on input fence");
                return NULL;
            }
        }
    } else {
        // TODO: remove this part.  Check if anyone uses this.

        if (srcSize < sizeof(VideoGrallocMetadata)) {
            ALOGE("Metadata is too small (%zu vs %zu)", srcSize, sizeof(VideoGrallocMetadata));
            return NULL;
        }

        VideoGrallocMetadata &grallocMeta = *(VideoGrallocMetadata *)(src);
        handle = grallocMeta.pHandle;
        // assume HAL_PIXEL_FORMAT_RGBA_8888
        // there is no way to get the src stride without the graphic buffer
        format = HAL_PIXEL_FORMAT_RGBA_8888;
        srcStride = width * 4;
        srcVStride = height;
    }

    size_t neededSize =
        dstStride * dstVStride + (width >> 1)
                + (dstStride >> 1) * ((dstVStride >> 1) + (height >> 1) - 1);
    if (dstSize < neededSize) {
        ALOGE("destination buffer is too small (%zu vs %zu)", dstSize, neededSize);
        return NULL;
    }

    auto& mapper = GraphicBufferMapper::get();

    void *bits = NULL;
    struct android_ycbcr ycbcr;
    status_t res;
    if (format == HAL_PIXEL_FORMAT_YCbCr_420_888) {
        res = mapper.lockYCbCr(
                 handle,
                 GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_NEVER,
                 Rect(width, height), &ycbcr);
    } else {
        res = mapper.lock(
                 handle,
                 GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_NEVER,
                 Rect(width, height), &bits);
    }
    if (res != OK) {
        ALOGE("Unable to lock image buffer %p for access", handle);
        return NULL;
    }

    //ALOGE("SoftVideoEncoderOMXComponent::extractGraphicBuffer format: %d", format);
    switch (format) {
        case HAL_PIXEL_FORMAT_YV12:  // YCrCb / YVU planar
            ycbcr.y = bits;
            ycbcr.cr = (uint8_t *)bits + srcStride * srcVStride;
            ycbcr.cb = (uint8_t *)ycbcr.cr + (srcStride >> 1) * (srcVStride >> 1);
            ycbcr.chroma_step = 1;
            ycbcr.cstride = srcStride >> 1;
            ycbcr.ystride = srcStride;
            ConvertFlexYUVToPlanar(dst, dstStride, dstVStride, &ycbcr, width, height);
            break;
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:  // YCrCb / YVU semiplanar, NV21
            ycbcr.y = bits;
            ycbcr.cr = (uint8_t *)bits + srcStride * srcVStride;
            ycbcr.cb = (uint8_t *)ycbcr.cr + 1;
            ycbcr.chroma_step = 2;
            ycbcr.cstride = srcStride;
            ycbcr.ystride = srcStride;
            ConvertFlexYUVToPlanar(dst, dstStride, dstVStride, &ycbcr, width, height);
            break;
        case HAL_PIXEL_FORMAT_YCbCr_420_888:  // YCbCr / YUV planar
            if (mIsPowervr) {
                if (mEglDisplay != EGL_NO_DISPLAY) {
                    VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)src;
                    ANativeWindowBuffer *buffer = nativeMeta.pBuffer;
                    auto image = eglCreateImageKHR(mEglDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                        (EGLClientBuffer) buffer, 0);
                    GLuint texture;
                    glGenTextures(1, &texture);
                    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
                    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)image);

                    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, mShmData);

                    ConvertRGB32ToPlanar(dst, dstStride, dstVStride, (const uint8_t *)mShmData, width,
                        height, srcStride, format == HAL_PIXEL_FORMAT_BGRA_8888);

                    glDeleteTextures(1, &texture);
                    eglDestroyImageKHR(mEglDisplay, image);
                } else {
                    ALOGE("mEglDisplay not initialized.");
                    return NULL;
                }
            } else {
                ConvertFlexYUVToPlanar(dst, dstStride, dstVStride, &ycbcr, width, height);
            }
            break;
        case HAL_PIXEL_FORMAT_RGBX_8888:
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            if(mIsPowervr){
                if(mEglDisplay != EGL_NO_DISPLAY){
                    EGLint image_attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };

                    VideoNativeMetadata &nativeMeta = *(VideoNativeMetadata *)src;
                    ANativeWindowBuffer *buffer = nativeMeta.pBuffer;
                    auto image = eglCreateImageKHR(mEglDisplay, EGL_NO_CONTEXT,
                                                EGL_NATIVE_BUFFER_ANDROID, (EGLClientBuffer) buffer,
                                                image_attrs);
                    GLuint texture;
                    glGenTextures(1, &texture);
                    glBindTexture(GL_TEXTURE_2D, texture);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);

                    drawQuad(0, 0, width, height);

                    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, mShmData);

                    ConvertRGB32ToPlanar(
                        dst, dstStride, dstVStride,
                        (const uint8_t *)mShmData, width, height, srcStride,
                        format == HAL_PIXEL_FORMAT_BGRA_8888);

                    glDeleteTextures(1, &texture);
                    eglDestroyImageKHR(mEglDisplay, image);
                }else{
                    ALOGE("mEglDisplay not initialized.");
                    return NULL;
                }
            }else{
                ConvertRGB32ToPlanar(
                    dst, dstStride, dstVStride,
                    (const uint8_t *)bits, width, height, srcStride,
                    format == HAL_PIXEL_FORMAT_BGRA_8888);
            }
            break;
        default:
            ALOGE("Unsupported pixel format %#x", format);
            dst = NULL;
            break;
    }

    if (mapper.unlock(handle) != OK) {
        ALOGE("Unable to unlock image buffer %p for access", handle);
    }

    return dst;
}

void SoftVideoEncoderOMXComponent::drawQuad(int x, int y, int w, int h) const {
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    GLint program;

    const GLfloat viewW = 0.5f * viewport[2];
    const GLfloat viewH = 0.5f * viewport[3];
    const GLfloat texW = 1.0f, texH = 1.0f;
    const GLfloat quadX1 = x       / viewW - 1.0f, quadY1 = y       / viewH - 1.0f;
    const GLfloat quadX2 = (x + w) / viewW - 1.0f, quadY2 = (y + h) / viewH - 1.0f;
    const GLfloat texcoords[] =
    {
         0,       0,
         0,       texH,
         texW,    0,
         texW,    texH
    };

    const GLfloat vertices[] =
    {
        quadX1, quadY1,
        quadX1, quadY2,
        quadX2, quadY1,
        quadX2, quadY2,
    };

    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    GLint positionAttr = glGetAttribLocation(program, "in_position");
    GLint texcoordAttr = glGetAttribLocation(program, "in_texcoord");

    glVertexAttribPointer(positionAttr, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glVertexAttribPointer(texcoordAttr, 2, GL_FLOAT, GL_FALSE, 0, texcoords);
    glEnableVertexAttribArray(positionAttr);
    glEnableVertexAttribArray(texcoordAttr);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

OMX_ERRORTYPE SoftVideoEncoderOMXComponent::getExtensionIndex(
        const char *name, OMX_INDEXTYPE *index) {
    if (!strcmp(name, "OMX.google.android.index.storeMetaDataInBuffers") ||
        !strcmp(name, "OMX.google.android.index.storeANWBufferInMetadata")) {
        *(int32_t*)index = kStoreMetaDataExtensionIndex;
        return OMX_ErrorNone;
    }
    return SimpleSoftOMXComponent::getExtensionIndex(name, index);
}

OMX_ERRORTYPE SoftVideoEncoderOMXComponent::validateInputBuffer(
        const OMX_BUFFERHEADERTYPE *inputBufferHeader) {
    size_t frameSize = mInputDataIsMeta ?
            max(sizeof(VideoNativeMetadata), sizeof(VideoGrallocMetadata))
            : mWidth * mHeight * 3 / 2;
    if (inputBufferHeader->nFilledLen < frameSize) {
        return OMX_ErrorUndefined;
    } else if (inputBufferHeader->nFilledLen > frameSize) {
        ALOGW("Input buffer contains more data than expected.");
    }
    return OMX_ErrorNone;
}

}  // namespace android
