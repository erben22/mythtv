#include <QFile>

#include <strings.h>

#include "mythlogging.h"
#include "openclinterface.h"
#include "videosurface.h"
#include "vdpauvideodecoder.h"
#include "openglsupport.h"

#ifdef MAX
#undef MAX
#endif
#ifdef MIN
#undef MIN
#endif
#include <oclUtils.h>

VideoSurface::VideoSurface(OpenCLDevice *dev, uint32_t width, uint32_t height,
                           uint id, VdpVideoSurface vdpSurface) : 
    m_id(id), m_dev(dev), m_width(width), m_height(height),
    m_vdpSurface(vdpSurface), m_valid(false)
{
    memset(&m_render, 0, sizeof(m_render));
    m_render.surface = m_vdpSurface;

    char *zeros = new char[m_width*m_height/2];

    glGenTextures(4, m_glVDPAUTex);
    glGenTextures(4, m_glOpenCLTex);
    glGenFramebuffersEXT(4, m_glFBO);

    m_glSurface = glVDPAURegisterVideoSurfaceNV((GLvoid*)(size_t)m_vdpSurface,
                                                GL_TEXTURE_RECTANGLE_ARB, 4,
                                                m_glVDPAUTex);

    for (int i = 0; i < 4; i++)
    {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, m_glVDPAUTex[i]);
        glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S,
                        GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T,
                        GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER,
                        GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER,
                        GL_NEAREST);

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_glFBO[i & 2]);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,
                                  GL_COLOR_ATTACHMENT0_EXT + (i & 1),
                                  GL_TEXTURE_RECTANGLE_ARB, m_glVDPAUTex[i], 0);

        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, m_glOpenCLTex[i]);
        glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_S,
                        GL_CLAMP_TO_EDGE);
        glTexParameterf(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_WRAP_T,
                        GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER,
                        GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER,
                        GL_NEAREST);

        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0,
                     (i < 2) ? GL_RED : GL_RG,
                     (i < 2) ? m_width : m_width / 2,
                     (i < 2) ? m_height / 2 : m_height / 4,
                     0, 
                     (i < 2) ? GL_RED : GL_RG,
                     GL_UNSIGNED_BYTE, 
                     zeros);

        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, 0);

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, m_glFBO[(i & 2) + 1]);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT,
                                  GL_COLOR_ATTACHMENT0_EXT + (i & 1),
                                  GL_TEXTURE_RECTANGLE_ARB, m_glOpenCLTex[i],
                                  0);

        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);

        cl_int ciErrNum;
        m_clBuffer[i] =
            clCreateFromGLTexture2D(m_dev->m_context, CL_MEM_READ_ONLY,
                                    GL_TEXTURE_RECTANGLE_ARB, 0,
                                    m_glOpenCLTex[i], &ciErrNum);
        if (ciErrNum != CL_SUCCESS)
        {
            LOG(VB_GENERAL, LOG_ERR,
                QString("VDPAU: OpenCL binding #%1 failed: %2 (%3)")
                .arg(i) .arg(ciErrNum) .arg(oclErrorString(ciErrNum)));
sleep(2);
            return;
        }
    }

    delete [] zeros;
    m_valid = true;
}

void VideoSurface::Bind(void)
{
    glVDPAUMapSurfacesNV(1, &m_glSurface);

    // Blit from the VDPAU-sourced textures to the OpenCL-reading textures
    for (int i = 0; i < 2; i++)
    {
        glBindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, m_glFBO[i*2]);
        glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, m_glFBO[(i*2)+1]);

        for (int j = 0; j < 2; j++)
        {
            GLenum bufnum = GL_COLOR_ATTACHMENT0_EXT + j;
            uint32_t width  = (i < 1) ? m_width : m_width / 2;
            uint32_t height = (i < 1) ? m_height / 2 : m_height / 4; 

            glReadBuffer(bufnum);
            glDrawBuffer(bufnum);
            glBlitFramebufferEXT(0, 0, width, height,
                                 0, 0, width, height,
                                 GL_COLOR_BUFFER_BIT,
                                 GL_NEAREST);
        }
    }

    glVDPAUUnmapSurfacesNV(1, &m_glSurface);
}

void VideoSurface::Dump(void)
{
    cl_int ciErrNum;

    for (int i = 0; i < 4; i++)
    {
        cl_image_format format;

        ciErrNum = clGetImageInfo(m_clBuffer[i], CL_IMAGE_FORMAT,
                                  sizeof(format), &format, NULL);
        LOG(VB_GENERAL, LOG_INFO,
            QString("Buffer %1: Format - Order %2, Type %3")
            .arg(i) .arg(oclImageFormatString(format.image_channel_order))
            .arg(oclImageFormatString(format.image_channel_data_type)));

        size_t elemSize, pitch, width, height;
        ciErrNum = clGetImageInfo(m_clBuffer[i], CL_IMAGE_ELEMENT_SIZE,
                                  sizeof(elemSize), &elemSize, NULL);
        LOG(VB_GENERAL, LOG_INFO, QString("Element Size: %1") .arg(elemSize));

        ciErrNum = clGetImageInfo(m_clBuffer[i], CL_IMAGE_ROW_PITCH,
                                  sizeof(pitch), &pitch, NULL);
        LOG(VB_GENERAL, LOG_INFO, QString("Row Pitch: %1") .arg(pitch));

        ciErrNum = clGetImageInfo(m_clBuffer[i], CL_IMAGE_WIDTH,
                                  sizeof(width), &width, NULL);
        ciErrNum = clGetImageInfo(m_clBuffer[i], CL_IMAGE_HEIGHT,
                                  sizeof(height), &height, NULL);
        LOG(VB_GENERAL, LOG_INFO, QString("Pixels: %1x%2") .arg(width)
            .arg(height));

        char *buf = new char[pitch*height];
        size_t origin[3] = {0, 0, 0};
        size_t region[3] = {width, height, 1};
        ciErrNum = clEnqueueReadImage(m_dev->m_commandQ, m_clBuffer[i], CL_TRUE,
                                      origin, region, pitch, 0, buf, NULL, 0,
                                      NULL);
        QString extension = (elemSize == 1) ? "ppm" : "bin";
        QString filename = QString("%1%2.%3").arg(basename).arg(i)
            .arg(extension);
        QFile file(filename);
        file.open(QIODevice::WriteOnly);
        if (elemSize == 1)
        {
            QString line = QString("P5\n%1 %2\n255\n").arg(width).arg(height);
            file.write(line.toLocal8Bit().constData());
        }
        file.write(buf, pitch*height);
        file.close();

        delete [] buf;
    }
}

VideoSurface::~VideoSurface()
{
    for (int i = 0; i < 4; i++)
    {
        if (m_clBuffer[i])
            clReleaseMemObject(m_clBuffer[i]);
    }

    glVDPAUUnregisterSurfaceNV(m_glSurface);
}

/*
 * vim:ts=4:sw=4:ai:et:si:sts=4
 */
