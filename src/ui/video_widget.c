/* vlx_video_widget.c — GtkGLArea zero-copy video renderer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GtkGLArea shares its EGL context with glimagesink so GStreamer writes
 * decoded frames directly into an OpenGL texture — no CPU blit, no memcpy.
 *
 * On each frame:
 *   1. GStreamer signals "client-draw" on glimagesink.
 *   2. We call gtk_gl_area_queue_render().
 *   3. GtkGLArea fires "render" — we draw a fullscreen quad with the texture.
 */

#include "ui/video_widget.h"
#include "utils/log.h"

#include <epoxy/gl.h>
#include <gst/gst.h>
#include <gst/gl/gl.h>

struct _VlxVideoWidget {
    GtkGLArea  parent_instance;

    GstElement *sink;         /* glimagesink */
    GstSample  *current_sample;
    GMutex      sample_lock;

    /* GL resources */
    GLuint      vao;
    GLuint      vbo;
    GLuint      program;
    GLuint      tex_uniform;
    gboolean    gl_ready;

    GLuint      cpu_texture_id;

    /* Screenshot request */
    gchar      *screenshot_path;
};

G_DEFINE_TYPE (VlxVideoWidget, vlx_video_widget, GTK_TYPE_GL_AREA)

/* ── Full-screen quad vertices (position + texcoords) ─────────────────── */
static const GLfloat QUAD_VERTS[] = {
    /* x      y      u     v   */
    -1.0f,  1.0f,  0.0f, 0.0f,
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
};

static const gchar *VERTEX_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "layout(location=1) in vec2 aTexCoord;\n"
    "out vec2 vTexCoord;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vTexCoord = aTexCoord;\n"
    "}\n";

static const gchar *FRAGMENT_SRC =
    "#version 330 core\n"
    "in vec2 vTexCoord;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTexture;\n"
    "void main() {\n"
    "  fragColor = texture(uTexture, vTexCoord);\n"
    "}\n";

/* ── Shader helpers ──────────────────────────────────────────────────── */
static GLuint
compile_shader (GLenum type, const gchar *src)
{
    GLuint shader = glCreateShader (type);
    glShaderSource (shader, 1, &src, NULL);
    glCompileShader (shader);

    GLint ok;
    glGetShaderiv (shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog (shader, sizeof (log), NULL, log);
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "Shader compile error: %s", log);
        glDeleteShader (shader);
        return 0;
    }
    return shader;
}

static GLuint
link_program (GLuint vert, GLuint frag)
{
    GLuint prog = glCreateProgram ();
    glAttachShader (prog, vert);
    glAttachShader (prog, frag);
    glLinkProgram (prog);

    GLint ok;
    glGetProgramiv (prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog (prog, sizeof (log), NULL, log);
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "Program link error: %s", log);
        glDeleteProgram (prog);
        return 0;
    }
    glDeleteShader (vert);
    glDeleteShader (frag);
    return prog;
}

/* ── GtkGLArea signals ───────────────────────────────────────────────── */
static void
on_realize (GtkGLArea *area)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (area);
    gtk_gl_area_make_current (area);

    if (gtk_gl_area_get_error (area)) {
        VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "GL context error on realize");
        return;
    }

    GLuint vert = compile_shader (GL_VERTEX_SHADER,   VERTEX_SRC);
    GLuint frag = compile_shader (GL_FRAGMENT_SHADER, FRAGMENT_SRC);
    self->program = link_program (vert, frag);

    self->tex_uniform = glGetUniformLocation (self->program, "uTexture");

    glGenVertexArrays (1, &self->vao);
    glGenBuffers (1, &self->vbo);

    glBindVertexArray (self->vao);
    glBindBuffer (GL_ARRAY_BUFFER, self->vbo);
    glBufferData (GL_ARRAY_BUFFER, sizeof (QUAD_VERTS), QUAD_VERTS, GL_STATIC_DRAW);

    glEnableVertexAttribArray (0);
    glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE,
                           4 * sizeof (GLfloat), (void *) 0);
    glEnableVertexAttribArray (1);
    glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE,
                           4 * sizeof (GLfloat),
                           (void *) (2 * sizeof (GLfloat)));

    glBindVertexArray (0);
    self->gl_ready = TRUE;
    VLX_LOG_INFO (VLX_LOG_DOMAIN_UI, "GL context ready");
}

static void
on_unrealize (GtkGLArea *area)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (area);
    gtk_gl_area_make_current (area);

    glDeleteVertexArrays (1, &self->vao);
    glDeleteBuffers (1, &self->vbo);
    glDeleteProgram (self->program);
    if (self->cpu_texture_id) {
        glDeleteTextures (1, &self->cpu_texture_id);
        self->cpu_texture_id = 0;
    }
    self->gl_ready = FALSE;
}

static gboolean
on_render (GtkGLArea *area, GdkGLContext *context)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (area);
    (void) context;

    glClearColor (0.0f, 0.0f, 0.0f, 1.0f);
    glClear (GL_COLOR_BUFFER_BIT);

    if (!self->gl_ready) return TRUE;

    g_mutex_lock (&self->sample_lock);
    GstSample *sample = self->current_sample
        ? gst_sample_ref (self->current_sample) : NULL;
    g_mutex_unlock (&self->sample_lock);

    if (!sample) return TRUE;

    GstBuffer *buf = gst_sample_get_buffer (sample);
    GstMemory *mem = gst_buffer_peek_memory (buf, 0);

    if (gst_is_gl_memory (mem)) {
        GstCaps *caps = gst_sample_get_caps (sample);
        GstVideoInfo info;
        gst_video_info_from_caps (&info, caps);

        int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
        int widget_width = gtk_widget_get_width (GTK_WIDGET (self)) * scale;
        int widget_height = gtk_widget_get_height (GTK_WIDGET (self)) * scale;

        double video_aspect = (double) info.width / info.height;
        if (info.par_n > 0 && info.par_d > 0)
            video_aspect *= (double) info.par_n / info.par_d;
            
        double widget_aspect = (double) widget_width / widget_height;

        int render_width = widget_width;
        int render_height = widget_height;
        int render_x = 0;
        int render_y = 0;

        if (video_aspect > widget_aspect) {
            render_height = widget_width / video_aspect;
            render_y = (widget_height - render_height) / 2;
        } else {
            render_width = widget_height * video_aspect;
            render_x = (widget_width - render_width) / 2;
        }
        glViewport (render_x, render_y, render_width, render_height);

        GstGLMemory *gl_mem = (GstGLMemory *) mem;
        GLuint tex_id = gst_gl_memory_get_texture_id (gl_mem);

        glUseProgram (self->program);
        glActiveTexture (GL_TEXTURE0);
        glBindTexture (GL_TEXTURE_2D, tex_id);
        glUniform1i (self->tex_uniform, 0);

        glBindVertexArray (self->vao);
        glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray (0);
    } else {
        GstVideoInfo info;
        GstCaps *caps = gst_sample_get_caps (sample);
        gst_video_info_from_caps (&info, caps);

        int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
        int widget_width = gtk_widget_get_width (GTK_WIDGET (self)) * scale;
        int widget_height = gtk_widget_get_height (GTK_WIDGET (self)) * scale;

        double video_aspect = (double) info.width / info.height;
        if (info.par_n > 0 && info.par_d > 0)
            video_aspect *= (double) info.par_n / info.par_d;
            
        double widget_aspect = (double) widget_width / widget_height;

        int render_width = widget_width;
        int render_height = widget_height;
        int render_x = 0;
        int render_y = 0;

        if (video_aspect > widget_aspect) {
            render_height = widget_width / video_aspect;
            render_y = (widget_height - render_height) / 2;
        } else {
            render_width = widget_height * video_aspect;
            render_x = (widget_width - render_width) / 2;
        }
        glViewport (render_x, render_y, render_width, render_height);

        GstVideoFrame frame;
        if (gst_video_frame_map (&frame, &info, buf, GST_MAP_READ)) {
            if (!self->cpu_texture_id)
                glGenTextures (1, &self->cpu_texture_id);

            glActiveTexture (GL_TEXTURE0);
            glBindTexture (GL_TEXTURE_2D, self->cpu_texture_id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            
            glPixelStorei (GL_UNPACK_ROW_LENGTH, GST_VIDEO_FRAME_COMP_STRIDE (&frame, 0) / 4);
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA, info.width, info.height,
                          0, GL_RGBA, GL_UNSIGNED_BYTE, GST_VIDEO_FRAME_PLANE_DATA (&frame, 0));
            glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);

            glUseProgram (self->program);
            glUniform1i (self->tex_uniform, 0);

            glBindVertexArray (self->vao);
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray (0);

            gst_video_frame_unmap (&frame);
        }
    }

    if (self->screenshot_path) {
        int scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
        int w = gtk_widget_get_width (GTK_WIDGET (self)) * scale;
        int h = gtk_widget_get_height (GTK_WIDGET (self)) * scale;

        guchar *pixels = g_malloc (w * h * 4);
        glReadPixels (0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        /* OpenGL bottom-up to GTK top-down */
        int stride = w * 4;
        guchar *flipped = g_malloc (h * stride);
        for (int y = 0; y < h; y++) {
            memcpy (flipped + (h - 1 - y) * stride, pixels + y * stride, stride);
        }
        g_free (pixels);

        GdkPixbufDestroyNotify free_pixels = (GdkPixbufDestroyNotify) (void (*)(void)) g_free;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data (flipped, GDK_COLORSPACE_RGB, TRUE, 8,
                                                      w, h, stride, free_pixels, NULL);
        GError *err = NULL;
        if (!gdk_pixbuf_save (pixbuf, self->screenshot_path, "png", &err, NULL)) {
            VLX_LOG_ERROR (VLX_LOG_DOMAIN_UI, "Failed to save screenshot: %s", err->message);
            g_error_free (err);
        }
        g_object_unref (pixbuf);
        g_free (self->screenshot_path);
        self->screenshot_path = NULL;
    }

    gst_sample_unref (sample);
    return TRUE;
}

/* ── GStreamer "new-sample" appsink callback ─────────────────────────── */
static GstFlowReturn
on_new_sample (GstElement *sink, gpointer data)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (data);
    (void) sink;

    GstSample *sample = NULL;
    g_signal_emit_by_name (sink, "pull-sample", &sample);
    if (!sample) return GST_FLOW_ERROR;

    g_mutex_lock (&self->sample_lock);
    g_clear_pointer (&self->current_sample, gst_sample_unref);
    self->current_sample = sample;
    g_mutex_unlock (&self->sample_lock);

    gtk_gl_area_queue_render (GTK_GL_AREA (self));
    return GST_FLOW_OK;
}

/* ── GObject boilerplate ─────────────────────────────────────────────── */
static void
vlx_video_widget_dispose (GObject *obj)
{
    VlxVideoWidget *self = VLX_VIDEO_WIDGET (obj);
    if (self->sink) {
        g_signal_handlers_disconnect_by_data (self->sink, self);
        g_clear_object (&self->sink);
    }
    g_clear_pointer (&self->current_sample, gst_sample_unref);
    g_mutex_clear (&self->sample_lock);
    G_OBJECT_CLASS (vlx_video_widget_parent_class)->dispose (obj);
}

static void
vlx_video_widget_class_init (VlxVideoWidgetClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = vlx_video_widget_dispose;
}

static void
vlx_video_widget_init (VlxVideoWidget *self)
{
    g_mutex_init (&self->sample_lock);
    gtk_gl_area_set_allowed_apis (GTK_GL_AREA (self), GDK_GL_API_GL);
    gtk_gl_area_set_has_depth_buffer (GTK_GL_AREA (self), FALSE);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    g_signal_connect (self, "realize",   G_CALLBACK (on_realize),   NULL);
    g_signal_connect (self, "unrealize", G_CALLBACK (on_unrealize), NULL);
    g_signal_connect (self, "render",    G_CALLBACK (on_render),    NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */
VlxVideoWidget *
vlx_video_widget_new (void)
{
    return g_object_new (VLX_TYPE_VIDEO_WIDGET, NULL);
}

void
vlx_video_widget_set_sink (VlxVideoWidget *self, GstElement *sink)
{
    g_return_if_fail (VLX_IS_VIDEO_WIDGET (self));

    if (self->sink) {
        g_signal_handlers_disconnect_by_data (self->sink, self);
        g_object_unref (self->sink);
        self->sink = NULL;
    }

    if (sink) {
        self->sink = g_object_ref (sink);
        g_object_set (self->sink, "emit-signals", TRUE, NULL);
        g_signal_connect (self->sink, "new-sample",
                          G_CALLBACK (on_new_sample), self);
    }
}

void
vlx_video_widget_take_screenshot (VlxVideoWidget *self, const gchar *path)
{
    g_return_if_fail (VLX_IS_VIDEO_WIDGET (self));
    g_return_if_fail (path != NULL);

    g_free (self->screenshot_path);
    self->screenshot_path = g_strdup (path);
    gtk_gl_area_queue_render (GTK_GL_AREA (self));
}

GdkTexture *
vlx_video_widget_get_current_texture (VlxVideoWidget *self)
{
    g_return_val_if_fail (VLX_IS_VIDEO_WIDGET (self), NULL);

    g_mutex_lock (&self->sample_lock);
    GstSample *sample = self->current_sample
        ? gst_sample_ref (self->current_sample) : NULL;
    g_mutex_unlock (&self->sample_lock);

    if (!sample) return NULL;

    GstCaps *caps = gst_sample_get_caps (sample);
    GstVideoInfo vinfo;
    if (!gst_video_info_from_caps (&vinfo, caps)) {
        gst_sample_unref (sample);
        return NULL;
    }

    GstBuffer *buf = gst_sample_get_buffer (sample);
    GstVideoFrame frame;
    if (!gst_video_frame_map (&frame, &vinfo, buf, GST_MAP_READ)) {
        gst_sample_unref (sample);
        return NULL;
    }

    int w = GST_VIDEO_FRAME_WIDTH (&frame);
    int h = GST_VIDEO_FRAME_HEIGHT (&frame);
    gsize stride = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);
    guint8 *pixels = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);

    GBytes *bytes = g_bytes_new (pixels, stride * h);
    GdkTexture *tex = gdk_memory_texture_new (w, h,
                                               GDK_MEMORY_R8G8B8A8,
                                               bytes, stride);
    g_bytes_unref (bytes);
    gst_video_frame_unmap (&frame);
    gst_sample_unref (sample);
    return tex;
}
