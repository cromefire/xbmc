/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include <algorithm>

#include "Picture.h"
#include "URL.h"
#include "ServiceBroker.h"
#include "settings/AdvancedSettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "FileItem.h"
#include "filesystem/File.h"
#include "utils/log.h"
#include "utils/URIUtils.h"
#include "guilib/Texture.h"
#include "guilib/imagefactory.h"

extern "C" {
#include <libswscale/swscale.h>
}

using namespace XFILE;

bool CPicture::GetThumbnailFromSurface(const unsigned char* buffer, int width, int height, int stride, const std::string &thumbFile, uint8_t* &result, size_t& result_size)
{
  unsigned char *thumb = NULL;
  unsigned int thumbsize = 0;

  // get an image handler
  IImage* image = ImageFactory::CreateLoader(thumbFile);
  if (image == NULL ||
      !image->CreateThumbnailFromSurface(const_cast<unsigned char*>(buffer), width, height,
                                         XB_FMT_A8R8G8B8, stride, thumbFile, thumb, thumbsize))
  {
    delete image;
    return false;
  }

  // copy the resulting buffer
  result_size = thumbsize;
  result = new uint8_t[result_size];
  memcpy(result, thumb, result_size);

  // release the image buffer and the image handler
  image->ReleaseThumbnailBuffer();
  delete image;

  return true;
}

bool CPicture::CreateThumbnailFromSurface(const unsigned char *buffer, int width, int height, int stride, const std::string &thumbFile)
{
  CLog::Log(LOGDEBUG, "cached image '{}' size {}x{}", CURL::GetRedacted(thumbFile), width, height);

  unsigned char *thumb = NULL;
  unsigned int thumbsize=0;
  IImage* pImage = ImageFactory::CreateLoader(thumbFile);
  if(pImage == NULL || !pImage->CreateThumbnailFromSurface(const_cast<unsigned char*>(buffer), width, height, XB_FMT_A8R8G8B8, stride, thumbFile.c_str(), thumb, thumbsize))
  {
    CLog::Log(LOGERROR, "Failed to CreateThumbnailFromSurface for {}",
              CURL::GetRedacted(thumbFile));
    delete pImage;
    return false;
  }

  XFILE::CFile file;
  const bool ret = file.OpenForWrite(thumbFile, true) &&
                   file.Write(thumb, thumbsize) == static_cast<ssize_t>(thumbsize);

  pImage->ReleaseThumbnailBuffer();
  delete pImage;

  return ret;
}

CThumbnailWriter::CThumbnailWriter(unsigned char* buffer, int width, int height, int stride, const std::string& thumbFile):
  m_thumbFile(thumbFile)
{
  m_buffer    = buffer;
  m_width     = width;
  m_height    = height;
  m_stride    = stride;
}

CThumbnailWriter::~CThumbnailWriter()
{
  delete m_buffer;
}

bool CThumbnailWriter::DoWork()
{
  bool success = true;

  if (!CPicture::CreateThumbnailFromSurface(m_buffer, m_width, m_height, m_stride, m_thumbFile))
  {
    CLog::Log(LOGERROR, "CThumbnailWriter::DoWork unable to write {}",
              CURL::GetRedacted(m_thumbFile));
    success = false;
  }

  delete [] m_buffer;
  m_buffer = NULL;

  return success;
}

bool CPicture::ResizeTexture(const std::string& image,
                             CTexture* texture,
                             uint32_t& dest_width,
                             uint32_t& dest_height,
                             uint8_t*& result,
                             size_t& result_size,
                             CPictureScalingAlgorithm::Algorithm
                                 scalingAlgorithm /* = CPictureScalingAlgorithm::NoAlgorithm */)
{
  if (image.empty() || texture == NULL)
    return false;

  return ResizeTexture(image, texture->GetPixels(), texture->GetWidth(), texture->GetHeight(), texture->GetPitch(),
                       dest_width, dest_height, result, result_size,
                       scalingAlgorithm);
}

bool CPicture::ResizeTexture(const std::string &image, uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch,
  uint32_t &dest_width, uint32_t &dest_height, uint8_t* &result, size_t& result_size,
  CPictureScalingAlgorithm::Algorithm scalingAlgorithm /* = CPictureScalingAlgorithm::NoAlgorithm */)
{
  if (image.empty() || pixels == NULL)
    return false;

  dest_width = std::min(width, dest_width);
  dest_height = std::min(height, dest_height);

  // if no max width or height is specified, don't resize
  if (dest_width == 0 && dest_height == 0)
  {
    dest_width = width;
    dest_height = height;
  }
  else if (dest_width == 0)
  {
    double factor = (double)dest_height / (double)height;
    dest_width = (uint32_t)(width * factor);
  }
  else if (dest_height == 0)
  {
    double factor = (double)dest_width / (double)width;
    dest_height = (uint32_t)(height * factor);
  }

  // nothing special to do if the dimensions already match
  if (dest_width >= width || dest_height >= height)
    return GetThumbnailFromSurface(pixels, dest_width, dest_height, pitch, image, result, result_size);

  // create a buffer large enough for the resulting image
  GetScale(width, height, dest_width, dest_height);

  uint8_t *buffer = new uint8_t[dest_width * dest_height * sizeof(uint32_t)];
  if (buffer == NULL)
  {
    result = NULL;
    result_size = 0;
    return false;
  }

  if (!ScaleImage(pixels, width, height, pitch, buffer, dest_width, dest_height, dest_width * sizeof(uint32_t), scalingAlgorithm))
  {
    delete[] buffer;
    result = NULL;
    result_size = 0;
    return false;
  }

  bool success = GetThumbnailFromSurface(buffer, dest_width, dest_height, dest_width * sizeof(uint32_t), image, result, result_size);
  delete[] buffer;

  if (!success)
  {
    result = NULL;
    result_size = 0;
  }

  return success;
}

bool CPicture::CacheTexture(CTexture* texture,
                            uint32_t& dest_width,
                            uint32_t& dest_height,
                            const std::string& dest,
                            CPictureScalingAlgorithm::Algorithm
                                scalingAlgorithm /* = CPictureScalingAlgorithm::NoAlgorithm */)
{
  return CacheTexture(texture->GetPixels(), texture->GetWidth(), texture->GetHeight(), texture->GetPitch(),
                      texture->GetOrientation(), dest_width, dest_height, dest, scalingAlgorithm);
}

bool CPicture::CacheTexture(uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch, int orientation,
  uint32_t &dest_width, uint32_t &dest_height, const std::string &dest,
  CPictureScalingAlgorithm::Algorithm scalingAlgorithm /* = CPictureScalingAlgorithm::NoAlgorithm */)
{
  const std::shared_ptr<CAdvancedSettings> advancedSettings = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings();

  // if no max width or height is specified, don't resize
  if (dest_width == 0)
    dest_width = width;
  if (dest_height == 0)
    dest_height = height;
  if (scalingAlgorithm == CPictureScalingAlgorithm::NoAlgorithm)
    scalingAlgorithm = advancedSettings->m_imageScalingAlgorithm;

  uint32_t max_height = advancedSettings->m_imageRes;
  if (advancedSettings->m_fanartRes > advancedSettings->m_imageRes)
  { // 16x9 images larger than the fanart res use that rather than the image res
    if (fabsf(static_cast<float>(width) / static_cast<float>(height) / (16.0f / 9.0f) - 1.0f)
        <= 0.01f)
    {
      max_height = advancedSettings->m_fanartRes; // use height defined in fanartRes
    }
  }

  uint32_t max_width = max_height * 16/9;

  dest_height = std::min(dest_height, max_height);
  dest_width  = std::min(dest_width, max_width);

  if (width > dest_width || height > dest_height || orientation)
  {
    bool success = false;

    dest_width = std::min(width, dest_width);
    dest_height = std::min(height, dest_height);

    // create a buffer large enough for the resulting image
    GetScale(width, height, dest_width, dest_height);
    uint32_t *buffer = new uint32_t[dest_width * dest_height];
    if (buffer)
    {
      if (ScaleImage(pixels, width, height, pitch,
                     (uint8_t *)buffer, dest_width, dest_height, dest_width * 4,
                     scalingAlgorithm))
      {
        if (!orientation || OrientateImage(buffer, dest_width, dest_height, orientation))
        {
          success = CreateThumbnailFromSurface((unsigned char*)buffer, dest_width, dest_height, dest_width * 4, dest);
        }
      }
      delete[] buffer;
    }
    return success;
  }
  else
  { // no orientation needed
    dest_width = width;
    dest_height = height;
    return CreateThumbnailFromSurface(pixels, width, height, pitch, dest);
  }
  return false;
}

bool CPicture::CreateTiledThumb(const std::vector<std::string> &files, const std::string &thumb)
{
  if (!files.size())
    return false;

  unsigned int num_across = (unsigned int)ceil(sqrt((float)files.size()));
  unsigned int num_down = (files.size() + num_across - 1) / num_across;

  unsigned int imageRes = CServiceBroker::GetSettingsComponent()->GetAdvancedSettings()->m_imageRes;

  unsigned int tile_width = imageRes / num_across;
  unsigned int tile_height = imageRes / num_down;
  unsigned int tile_gap = 1;
  bool success = false;

  // create a buffer for the resulting thumb
  uint32_t *buffer = static_cast<uint32_t *>(calloc(imageRes * imageRes, 4));
  if (!buffer)
    return false;
  for (unsigned int i = 0; i < files.size(); ++i)
  {
    int x = i % num_across;
    int y = i / num_across;
    // load in the image
    unsigned int width = tile_width - 2*tile_gap, height = tile_height - 2*tile_gap;
    std::unique_ptr<CTexture> texture = CTexture::LoadFromFile(files[i], width, height, true);
    if (texture && texture->GetWidth() && texture->GetHeight())
    {
      GetScale(texture->GetWidth(), texture->GetHeight(), width, height);

      // scale appropriately
      uint32_t *scaled = new uint32_t[width * height];
      if (ScaleImage(texture->GetPixels(), texture->GetWidth(), texture->GetHeight(), texture->GetPitch(),
                     (uint8_t *)scaled, width, height, width * 4))
      {
        if (!texture->GetOrientation() || OrientateImage(scaled, width, height, texture->GetOrientation()))
        {
          success = true; // Flag that we at least had one successful image processed
          // drop into the texture
          unsigned int posX = x*tile_width + (tile_width - width)/2;
          unsigned int posY = y*tile_height + (tile_height - height)/2;
          uint32_t *dest = buffer + posX + posY * imageRes;
          uint32_t *src = scaled;
          for (unsigned int y = 0; y < height; ++y)
          {
            memcpy(dest, src, width*4);
            dest += imageRes;
            src += width;
          }
        }
      }
      delete[] scaled;
    }
  }
  // now save to a file
  if (success)
    success = CreateThumbnailFromSurface((uint8_t *)buffer, imageRes, imageRes, imageRes * 4, thumb);

  free(buffer);
  return success;
}

void CPicture::GetScale(unsigned int width, unsigned int height, unsigned int &out_width, unsigned int &out_height)
{
  float aspect = (float)width / height;
  if ((unsigned int)(out_width / aspect + 0.5f) > out_height)
    out_width = (unsigned int)(out_height * aspect + 0.5f);
  else
    out_height = (unsigned int)(out_width / aspect + 0.5f);
}

bool CPicture::ScaleImage(uint8_t *in_pixels, unsigned int in_width, unsigned int in_height, unsigned int in_pitch,
                          uint8_t *out_pixels, unsigned int out_width, unsigned int out_height, unsigned int out_pitch,
                          CPictureScalingAlgorithm::Algorithm scalingAlgorithm /* = CPictureScalingAlgorithm::NoAlgorithm */)
{
  struct SwsContext *context = sws_getContext(in_width, in_height, AV_PIX_FMT_BGRA,
                                                         out_width, out_height, AV_PIX_FMT_BGRA,
                                                         CPictureScalingAlgorithm::ToSwscale(scalingAlgorithm), NULL, NULL, NULL);

  uint8_t *src[] = { in_pixels, 0, 0, 0 };
  int     srcStride[] = { (int)in_pitch, 0, 0, 0 };
  uint8_t *dst[] = { out_pixels , 0, 0, 0 };
  int     dstStride[] = { (int)out_pitch, 0, 0, 0 };

  if (context)
  {
    sws_scale(context, src, srcStride, 0, in_height, dst, dstStride);
    sws_freeContext(context);
    return true;
  }
  return false;
}

bool CPicture::OrientateImage(uint32_t *&pixels, unsigned int &width, unsigned int &height, int orientation)
{
  // ideas for speeding these functions up: http://cgit.freedesktop.org/pixman/tree/pixman/pixman-fast-path.c
  bool out = false;
  switch (orientation)
  {
    case 1:
      out = FlipHorizontal(pixels, width, height);
      break;
    case 2:
      out = Rotate180CCW(pixels, width, height);
      break;
    case 3:
      out = FlipVertical(pixels, width, height);
      break;
    case 4:
      out = Transpose(pixels, width, height);
      break;
    case 5:
      out = Rotate270CCW(pixels, width, height);
      break;
    case 6:
      out = TransposeOffAxis(pixels, width, height);
      break;
    case 7:
      out = Rotate90CCW(pixels, width, height);
      break;
    default:
      CLog::Log(LOGERROR, "Unknown orientation {}", orientation);
      break;
  }
  return out;
}

bool CPicture::FlipHorizontal(uint32_t*& pixels,
                              const unsigned int& width,
                              const unsigned int& height)
{
  // this can be done in-place easily enough
  for (unsigned int y = 0; y < height; ++y)
  {
    uint32_t *line = pixels + y * width;
    for (unsigned int x = 0; x < width / 2; ++x)
      std::swap(line[x], line[width - 1 - x]);
  }
  return true;
}

bool CPicture::FlipVertical(uint32_t*& pixels,
                            const unsigned int& width,
                            const unsigned int& height)
{
  // this can be done in-place easily enough
  for (unsigned int y = 0; y < height / 2; ++y)
  {
    uint32_t *line1 = pixels + y * width;
    uint32_t *line2 = pixels + (height - 1 - y) * width;
    for (unsigned int x = 0; x < width; ++x)
      std::swap(*line1++, *line2++);
  }
  return true;
}

bool CPicture::Rotate180CCW(uint32_t*& pixels,
                            const unsigned int& width,
                            const unsigned int& height)
{
  // this can be done in-place easily enough
  for (unsigned int y = 0; y < height / 2; ++y)
  {
    uint32_t *line1 = pixels + y * width;
    uint32_t *line2 = pixels + (height - 1 - y) * width + width - 1;
    for (unsigned int x = 0; x < width; ++x)
      std::swap(*line1++, *line2--);
  }
  if (height % 2)
  { // height is odd, so flip the middle row as well
    uint32_t *line = pixels + (height - 1)/2 * width;
    for (unsigned int x = 0; x < width / 2; ++x)
      std::swap(line[x], line[width - 1 - x]);
  }
  return true;
}

bool CPicture::Rotate90CCW(uint32_t *&pixels, unsigned int &width, unsigned int &height)
{
  uint32_t *dest = new uint32_t[width * height * 4];
  if (dest)
  {
    unsigned int d_height = width, d_width = height;
    for (unsigned int y = 0; y < d_height; y++)
    {
      const uint32_t *src = pixels + (d_height - 1 - y); // y-th col from right, starting at top
      uint32_t *dst = dest + d_width * y;                // y-th row from top, starting at left
      for (unsigned int x = 0; x < d_width; x++)
      {
        *dst++ = *src;
        src += width;
      }
    }
    delete[] pixels;
    pixels = dest;
    std::swap(width, height);
    return true;
  }
  return false;
}

bool CPicture::Rotate270CCW(uint32_t *&pixels, unsigned int &width, unsigned int &height)
{
  uint32_t *dest = new uint32_t[width * height * 4];
  if (!dest)
    return false;

  unsigned int d_height = width, d_width = height;
  for (unsigned int y = 0; y < d_height; y++)
  {
    const uint32_t *src = pixels + width * (d_width - 1) + y; // y-th col from left, starting at bottom
    uint32_t *dst = dest + d_width * y;                       // y-th row from top, starting at left
    for (unsigned int x = 0; x < d_width; x++)
    {
      *dst++ = *src;
      src -= width;
    }
  }

  delete[] pixels;
  pixels = dest;
  std::swap(width, height);
  return true;
}

bool CPicture::Transpose(uint32_t *&pixels, unsigned int &width, unsigned int &height)
{
  uint32_t *dest = new uint32_t[width * height * 4];
  if (!dest)
    return false;

  unsigned int d_height = width, d_width = height;
  for (unsigned int y = 0; y < d_height; y++)
  {
    const uint32_t *src = pixels + y;   // y-th col from left, starting at top
    uint32_t *dst = dest + d_width * y; // y-th row from top, starting at left
    for (unsigned int x = 0; x < d_width; x++)
    {
      *dst++ = *src;
      src += width;
    }
  }

  delete[] pixels;
  pixels = dest;
  std::swap(width, height);
  return true;
}

bool CPicture::TransposeOffAxis(uint32_t *&pixels, unsigned int &width, unsigned int &height)
{
  uint32_t *dest = new uint32_t[width * height * 4];
  if (!dest)
    return false;

  unsigned int d_height = width, d_width = height;
  for (unsigned int y = 0; y < d_height; y++)
  {
    const uint32_t *src = pixels + width * (d_width - 1) + (d_height - 1 - y); // y-th col from right, starting at bottom
    uint32_t *dst = dest + d_width * y;                                        // y-th row, starting at left
    for (unsigned int x = 0; x < d_width; x++)
    {
      *dst++ = *src;
      src -= width;
    }
  }

  delete[] pixels;
  pixels = dest;
  std::swap(width, height);
  return true;
}
