//========================================================================
// GLFW 3.5 Wayland - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2014 Jonas Ådahl <jadahl@gmail.com>
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================
/*
_glfwCreateWindowKMSDRM
    _glfwInitEGL
        _glfw.egl.display = eglGetDisplay(_glfw.platform.getEGLNativeDisplay());
        if (!eglInitialize(_glfw.egl.display, &_glfw.egl.major, &_glfw.egl.minor)) {
    _glfwCreateContextEGL
        if (!chooseEGLConfig(ctxconfig, fbconfig, &config))
        if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        window->context.egl.handle = eglCreateContext(_glfw.egl.display,config, share, attribs);
        window->context.egl.surface = eglCreateWindowSurface(_glfw.egl.display, config, native, attribs);
    _glfwRefreshContextAttribs
        glfwMakeContextCurrent((GLFWwindow*) window);
        window->context.swapBuffers(window); => static void swapBuffersEGL(_GLFWwindow* window) {
            gbm_surface_release_buffer(window->context.egl.surface, _glfw.kmsdrm.gbm.bo);
            eglSwapBuffers(_glfw.egl.display, window->context.egl.surface);
            _glfw.kmsdrm.next_bo = gbm_surface_lock_front_buffer(window->context.egl.surface);
            void* fb_id_ptr = gbm_bo_get_user_data(_glfw.kmsdrm.next_bo);
            unsigned w = gbm_bo_get_width(_glfw.kmsdrm.next_bo);
            unsigned h = gbm_bo_get_height(_glfw.kmsdrm.next_bo);
            uint32_t strides = gbm_bo_get_stride(_glfw.kmsdrm.next_bo);
            uint32_t handles = gbm_bo_get_handle(_glfw.kmsdrm.next_bo).u32;
            int rc = drmModeAddFB(_glfw.kmsdrm.drm_fd, w, h, 24, 32, strides, handles, &fb_id);
            gbm_bo_set_user_data(_glfw.kmsdrm.next_bo, (void*) &fb_id, KMSDRM_FBDestroyCallback); // Associate our DRM framebuffer with this buffer object
            int ret = drmModeSetCrtc(_glfw.kmsdrm.drm_fd,
            OR
            int ret = drmModePageFlip(_glfw.kmsdrm.drm_fd, _glfw.kmsdrm.encoder->crtc_id,fb_id, flip_flags, &(_glfw.kmsdrm.waiting_for_flip));
        glfwMakeContextCurrent((GLFWwindow*) previous);
*/

#define _GNU_SOURCE

#include "internal.h"

#if defined(_GLFW_KMSDRM)

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

int init_surface(struct gbm* gbm, uint64_t modifier) {
    debug_printf("init_surface: gbm_surface_create_with_modifiers gbm.device:%p gbm.width=%d gbm.height=%d, gbm.format=%d modifier=%ld\n", gbm->dev, gbm->width, gbm->height, gbm->format, modifier);
    gbm->surface = gbm_surface_create_with_modifiers(gbm->dev, gbm->width, gbm->height, gbm->format, &modifier, 1);

    if (!gbm->surface) {
        if (modifier != DRM_FORMAT_MOD_LINEAR) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "init_surface: Modifiers requested but support isn't available\n");
            return -2;
        }
        debug_printf("init_surface: gbm_surface_create gbm.device:%p gbm.width=%d gbm.height=%d, gbm.format=%d modifier=%d", gbm->dev, gbm->width, gbm->height, gbm->format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        gbm->surface = gbm_surface_create(gbm->dev, gbm->width, gbm->height, gbm->format, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }
    if (!gbm->surface) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "init_surface: Failed to create gbm surface\n");
        return -3;
    }
    printf("init_surface: %dx%d created\n", gbm->width, gbm->height);
    return 0;
}

int init_gbm(struct gbm* gbm, int drm_fd, int w, int h, uint32_t format, uint64_t modifier) {
    debug_printf("init_gbm: gbm_create_device drm_fd=%d\n", drm_fd);
    gbm->dev = gbm_create_device(drm_fd);
    if (!gbm->dev)
        return -1;

    gbm->format = format;
    gbm->surface = NULL;

    debug_printf("init_gbm: request width=%d height=%d\n", w, h);
    if (w && h) {
        gbm->width = w;
        gbm->height = h;
    } else {
        gbm->width = _glfw.kmsdrm.drm.mode->hdisplay;
        gbm->height = _glfw.kmsdrm.drm.mode->vdisplay;
    }

    return init_surface(gbm, modifier);
}

static void handleEvents(double* timeout) {
#if defined(GLFW_BUILD_LINUX_JOYSTICK)
    if (_glfw.joysticksInitialized)
        _glfwDetectJoystickConnectionLinux();
#endif
// #if defined(GLFW_BUILD_LINUX_KEYBOARD)
//     if (_glfw.keyboardsInitialized)
//         _glfwDetectKeyboardConnectionLinux();
// #endif

    GLFWbool event = GLFW_FALSE;
    struct pollfd fds[1];

    if (_glfw.kmsdrm.keyboard_fd > 0) {
        fds[0].fd = _glfw.kmsdrm.keyboard_fd;
        fds[0].events = POLLIN;
    }

    GLFWbool ret = 0;
    while (!event) {
        ret = _glfwPollPOSIX(fds, sizeof(fds) / sizeof(fds[0]), timeout);
        if (ret == -1) { // Poll error
            perror("poll");
            return;
        } else if (ret == 0) { // Timeout occurred! No data
            return;
        }

        // Data available then handle event
        if (fds[0].revents & POLLIN) {
            _glfwPollKeyboardLinux();
            event = GLFW_TRUE;
        }
    }
}

EGLenum _glfwGetEGLPlatformKMSDRM(EGLint** attribs) {
    if (_glfw.egl.EXT_platform_base && _glfw.egl.EXT_platform_kmsdrm)
        return EGL_PLATFORM_GBM_MESA;
    else
        return 0;
}

EGLNativeDisplayType _glfwGetEGLNativeDisplayKMSDRM(void) {
    return _glfw.kmsdrm.gbm.dev;
}

EGLNativeWindowType _glfwGetEGLNativeWindowKMSDRM(_GLFWwindow* window) {
    // debug_printf("kmsdrm_window.c:%d _glfwGetEGLNativeWindowKMSDRM => %p\n", __LINE__, (void*) _glfw.kmsdrm.gbm.surface);
    // return _glfw.kmsdrm.egl.surface;
    return _glfw.kmsdrm.gbm.surface;
    // return window->context.egl.surface;
}

GLFWbool _glfwCreateWindowKMSDRM(_GLFWwindow* window, const _GLFWwndconfig* wndconfig, const _GLFWctxconfig* ctxconfig, const _GLFWfbconfig* fbconfig) {
    // debug_printf("kmsdrm_window.c:%d _glfwCreateWindowKMSDRM BEGIN\n", __LINE__);
    if (init_gbm(&_glfw.kmsdrm.gbm, _glfw.kmsdrm.drm.fd, wndconfig->width, wndconfig->height, _glfw.kmsdrm.format, _glfw.kmsdrm.modifier)) {
        debug_printf("Failed to initialize GBM.\n");
        return GLFW_FALSE;
    } else {
        debug_printf("Initializing GBM [OK]\n");
    }

    // Initialize EGL TODO: samakan _glfwInitEGL dengan init_egl
    if (ctxconfig->client != GLFW_NO_API) {
        if (ctxconfig->source == GLFW_EGL_CONTEXT_API || ctxconfig->source == GLFW_NATIVE_CONTEXT_API) {
            if (!_glfwInitEGL()) {
                _glfwInputError(GLFW_PLATFORM_ERROR, "_glfwCreateWindowKMSDRM: _glfwInitEGL failed\n");
                return GLFW_FALSE;
            }
        }
    }

    // Create Context (EGL)
    if (ctxconfig->client != GLFW_NO_API) {
        if (ctxconfig->source == GLFW_EGL_CONTEXT_API || ctxconfig->source == GLFW_NATIVE_CONTEXT_API) {
            if (!_glfwCreateContextEGL(window, ctxconfig, fbconfig)) {
                _glfwInputError(GLFW_PLATFORM_ERROR, "_glfwCreateWindowKMSDRM: _glfwCreateContextEGL failed\n");
                return GLFW_FALSE;
            }
        }

        if (!_glfwRefreshContextAttribs(window, ctxconfig)) {
            debug_printf("kmsdrm_window.c:%d _glfwRefreshContextAttribs failed\n", __LINE__);
            return GLFW_FALSE;
        } else {
            // debug_printf("kmsdrm_window.c:%d _glfwRefreshContextAttribs succeed\n", __LINE__);
        }
    }

    // initial eglSwap here
    debug_printf("_glfwCreateWindowKMSDRM: eglMakeCurrent egl.display=%p egl.surface=%p egl.context=%p\n", _glfw.egl.display, window->context.egl.surface, window->context.egl.handle);
    if (!eglMakeCurrent(_glfw.egl.display, window->context.egl.surface, window->context.egl.surface, window->context.egl.handle)) {
        debug_printf("EGL: Failed to make context current\n");
        return GLFW_FALSE;
    }
    if (_glfw.kmsdrm.gbm.surface) {
        debug_printf("_glfwCreateWindowKMSDRM: eglSwapBuffers egl.display=%p egl.surface=%p\n", _glfw.egl.display, window->context.egl.surface);
        if (!eglSwapBuffers(_glfw.egl.display, window->context.egl.surface)) {
            debug_printf("_glfwCreateWindowKMSDRM: eglSwapBuffers error\n");
            return GLFW_FALSE;
        }
        debug_puts("_glfwCreateWindowKMSDRM: gbm_surface_lock_front_buffer");
        _glfw.kmsdrm.gbm.bo = gbm_surface_lock_front_buffer(_glfw.kmsdrm.gbm.surface);
        if (!_glfw.kmsdrm.gbm.bo) {
            debug_printf("gbm_surface_lock_front_buffer error.\n");
            return GLFW_FALSE;
        }
    }
    debug_puts("_glfwCreateWindowKMSDRM: drm_fb_get_from_bo");
    _glfw.kmsdrm.gbm.fb = drm_fb_get_from_bo(_glfw.kmsdrm.gbm.bo);
    if (!_glfw.kmsdrm.gbm.fb) {
        debug_printf("Failed to get a new framebuffer BO\n");
        return GLFW_FALSE;
    }
    debug_printf("_glfwCreateWindowKMSDRM: drmModeSetCrtc drm.fd=%d drm.crtc_id=%d fb_id=%d drm.connector_id=%d drm.mode=%p\n", _glfw.kmsdrm.drm.fd, _glfw.kmsdrm.drm.crtc_id, _glfw.kmsdrm.gbm.fb->fb_id, _glfw.kmsdrm.drm.connector_id, _glfw.kmsdrm.drm.mode);
    if (drmModeSetCrtc(_glfw.kmsdrm.drm.fd, _glfw.kmsdrm.drm.crtc_id, _glfw.kmsdrm.gbm.fb->fb_id, 0, 0, &(_glfw.kmsdrm.drm.connector_id), 1, _glfw.kmsdrm.drm.mode)) {
        debug_printf("drmModeSetCrtc error: %s (%d)\n", strerror(errno), errno);
        return GLFW_FALSE;
    }

#if defined(GLFW_BUILD_LINUX_KEYBOARD)
    _glfw.kmsdrm.window = window;
#endif
    return GLFW_TRUE;
}

void _glfwDestroyWindowKMSDRM(_GLFWwindow* window) {
    // Clean up framebuffer, CRTC, etc.
    // drmModeSetCrtc(_glfw.kmsdrm.fd, window->kmsdrm.saved_crtc, 0, 0, 0, NULL, 0, NULL);
}

void _glfwGetFramebufferSizeKMSDRM(_GLFWwindow* window, int* width, int* height) {
    // puts("_glfwGetFramebufferSizeKMSDRM");
    if (width)
        *width = _glfw.kmsdrm.gbm.width; // *width = 640; //window->wl.fbWidth;
    if (height)
        *height = _glfw.kmsdrm.gbm.height; // *height = 480; //window->wl.fbHeight;
}

void _glfwPollEventsKMSDRM(void) {
    double timeout = 0.0;
    handleEvents(&timeout);
    // #if defined(GLFW_BUILD_LINUX_KEYBOARD)
    //     for (int jid = 0; jid <= GLFW_KEYBOARD_LAST; jid++) {
    //         _GLFWkeyboard* js = _glfw.keyboards + jid;
    //         if (js->connected)
    //             _glfwPollKeyboardLinux(js, _GLFW_POLL_ALL);
    //     }
    // #endif    
}
#endif

