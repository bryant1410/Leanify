#include "jpeg.h"

#include <algorithm>
#include <csetjmp>  // for mozjpeg error handling
#include <iostream>

#include <mozjpeg/jpeglib.h>

const uint8_t Jpeg::header_magic[] = { 0xFF, 0xD8, 0xFF };
bool Jpeg::keep_exif_ = false;
bool Jpeg::keep_icc_profile_ = false;
bool Jpeg::keep_all_metadata_ = false;
bool Jpeg::force_arithmetic_coding_ = false;

namespace {

jmp_buf setjmp_buffer;

void mozjpeg_error_handler(j_common_ptr cinfo) {
  (*cinfo->err->output_message)(cinfo);

  longjmp(setjmp_buffer, 1);
}

}  // namespace

size_t Jpeg::Leanify(size_t size_leanified /*= 0*/) {
  struct jpeg_decompress_struct srcinfo;
  struct jpeg_compress_struct dstinfo;
  struct jpeg_error_mgr jsrcerr, jdsterr;

  srcinfo.err = jpeg_std_error(&jsrcerr);
  jsrcerr.error_exit = mozjpeg_error_handler;
  if (setjmp(setjmp_buffer)) {
    jpeg_destroy_compress(&dstinfo);
    jpeg_destroy_decompress(&srcinfo);

    return Format::Leanify(size_leanified);
  }

  jpeg_create_decompress(&srcinfo);

  dstinfo.err = jpeg_std_error(&jdsterr);
  jdsterr.error_exit = mozjpeg_error_handler;

  jpeg_create_compress(&dstinfo);

  if (is_verbose) {
    dstinfo.err->trace_level++;
  }
  if (is_fast) {
    jpeg_c_set_int_param(&dstinfo, JINT_COMPRESS_PROFILE, JCP_FASTEST);
  }

  /* Specify data source for decompression */
  jpeg_mem_src(&srcinfo, fp_, size_);

  // Always save exif to show warning if orientation might change.
  jpeg_save_markers(&srcinfo, JPEG_APP0 + 1, 0xFFFF);
  if (keep_icc_profile_ || keep_all_metadata_) {
    jpeg_save_markers(&srcinfo, JPEG_APP0 + 2, 0xFFFF);
  }
  if (keep_all_metadata_) {
    // Save the rest APPn markers.
    for (int i = 3; i < 16; i++)
      jpeg_save_markers(&srcinfo, JPEG_APP0 + i, 0xFFFF);
    // Save comments.
    jpeg_save_markers(&srcinfo, JPEG_COM, 0xFFFF);
  }

  (void)jpeg_read_header(&srcinfo, true);

  /* Read source file as DCT coefficients */
  auto coef_arrays = jpeg_read_coefficients(&srcinfo);

  /* Initialize destination compression parameters from source values */
  jpeg_copy_critical_parameters(&srcinfo, &dstinfo);

  // use arithmetic coding if input file is arithmetic coded or if forced to
  if (srcinfo.arith_code || force_arithmetic_coding_) {
    dstinfo.arith_code = true;
    dstinfo.optimize_coding = false;
  } else {
    dstinfo.optimize_coding = true;
  }

  uint8_t* outbuffer = nullptr;
  unsigned long outsize = 0;
  /* Specify data destination for compression */
  jpeg_mem_dest(&dstinfo, &outbuffer, &outsize);

  /* Start compressor (note no image data is actually written here) */
  jpeg_write_coefficients(&dstinfo, coef_arrays);

  for (auto marker = srcinfo.marker_list; marker; marker = marker->next) {
    if (marker->marker == JPEG_APP0 + 1 && !keep_exif_ && !keep_all_metadata_) {
      // Tag number: 0x0112, data format: unsigned short(3), number of components: 1
      const uint8_t kExifOrientation[] = { 0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00 };
      const uint8_t kExifOrientationMotorola[] = { 0x01, 0x12, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01 };
      uint8_t* start = marker->data;
      uint8_t* end = start + marker->data_length;
      uint8_t* orientation_tag = std::search(start, end, kExifOrientation, std::end(kExifOrientation));
      bool big_endian = false;
      if (orientation_tag == end) {
        orientation_tag = std::search(start, end, kExifOrientationMotorola, std::end(kExifOrientationMotorola));
        big_endian = orientation_tag != end;
      }
      if (orientation_tag != end) {
        uint16_t orientation = *reinterpret_cast<uint16_t*>(orientation_tag + sizeof(kExifOrientation));
        if (big_endian)
          orientation = ((orientation >> 8) | (orientation << 8)) & 0xFFFF;
        // Only show warning if it's not the default upper left.
        if (orientation != 1) {
          std::cout << "Warning: The Exif being removed contains orientation data, result image might have wrong "
                       "orientation, use --keep-exif to keep Exif."
                    << std::endl;
        }
      }
      continue;
    }
    jpeg_write_marker(&dstinfo, marker->marker, marker->data, marker->data_length);
  }

  /* Finish compression and release memory */
  jpeg_finish_compress(&dstinfo);

  (void)jpeg_finish_decompress(&srcinfo);
  jpeg_destroy_decompress(&srcinfo);

  fp_ -= size_leanified;
  // use mozjpeg result if it's smaller than original
  if (outsize < size_) {
    memcpy(fp_, outbuffer, outsize);
    size_ = outsize;
  } else {
    memmove(fp_, fp_ + size_leanified, size_);
  }

  jpeg_destroy_compress(&dstinfo);

  return size_;
}