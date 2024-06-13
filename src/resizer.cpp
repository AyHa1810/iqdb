/***************************************************************************\
    resizer.cpp - Image resizer using libgd, using pre-scaled images
		  from libjpeg/libpng to be faster and use less memory.

    Copyright (C) 2008 piespy@gmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
\**************************************************************************/

#include <gd.h>
#include <memory>

#include <iqdb/debug.h>
#include <iqdb/imgdb.h>
#include <iqdb/resizer.h>

namespace iqdb {

enum image_types { IMG_UNKNOWN, IMG_JPEG, IMG_PNG, IMG_GIF, IMG_BMP };

image_types get_image_info(const unsigned char *data, size_t length) {
  if (length >= 3 && memcmp(data, "\xff\xd8\xff", 3) == 0) {
    return IMG_JPEG;
  } else if (length >= 4 && memcmp(data, "\x89\x50\x4e\x47", 4) == 0) {
    return IMG_PNG;
  } else if (length >= 3 && memcmp(data, "\x47\x49\x46", 3) == 0) {
    return IMG_GIF;
  } else if (length >= 2 && memcmp(data, "\x42\x4d", 2) == 0) {
    return IMG_BMP;
  } else {
    return IMG_UNKNOWN;
  }
}

RawImage get_raw_image(image_types type, size_t len, const unsigned char *data)
{
  switch (type)
  {
  case IMG_JPEG:
    return RawImage(gdImageCreateFromJpegPtr((int)len, const_cast<unsigned char *>(data)), &gdImageDestroy);
  case IMG_PNG:
    return RawImage(gdImageCreateFromPngPtr((int)len, const_cast<unsigned char *>(data)), &gdImageDestroy);
  case IMG_GIF:
    return RawImage(gdImageCreateFromGifPtr((int)len, const_cast<unsigned char *>(data)), &gdImageDestroy);
  case IMG_BMP:
    return RawImage(gdImageCreateFromBmpPtr((int)len, const_cast<unsigned char *>(data)), &gdImageDestroy);

  default:
    throw image_error("Unsupported image format.");
  }
}

RawImage resize_image_data(const unsigned char *data, size_t len, unsigned int thu_x, unsigned int thu_y) {
  auto type = get_image_info(data, len);

  RawImage thu(gdImageCreateTrueColor(thu_x, thu_y), &gdImageDestroy);
  if (!thu)
    throw image_error("Out of memory.");

  RawImage img = get_raw_image(type, len, data);
  if (!img)
    throw image_error("Could not read image.");

  if ((unsigned int)img->sx == thu_x && (unsigned int)img->sy == thu_y && gdImageTrueColor(img))
    return img;

  gdImageCopyResampled(thu.get(), img.get(), 0, 0, 0, 0, thu_x, thu_y, img->sx, img->sy);
  DEBUG("Resized {} x {} to {} x {}.\n", img->sx, img->sy, thu_x, thu_y);

  return thu;
}

}
