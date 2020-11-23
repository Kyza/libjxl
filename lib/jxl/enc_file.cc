// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "lib/jxl/enc_file.h"

#include <stddef.h>

#include <type_traits>
#include <utility>
#include <vector>

#include "lib/jxl/aux_out.h"
#include "lib/jxl/aux_out_fwd.h"
#include "lib/jxl/base/compiler_specific.h"
#include "lib/jxl/codec_in_out.h"
#include "lib/jxl/color_encoding_internal.h"
#include "lib/jxl/enc_bit_writer.h"
#include "lib/jxl/enc_cache.h"
#include "lib/jxl/enc_frame.h"
#include "lib/jxl/frame_header.h"
#include "lib/jxl/headers.h"
#include "lib/jxl/icc_codec.h"
#include "lib/jxl/image_bundle.h"

namespace jxl {

namespace {

// DC + 'Very Low Frequency'
PassDefinition progressive_passes_dc_vlf[] = {
    {/*num_coefficients=*/2, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/4}};

PassDefinition progressive_passes_dc_lf[] = {
    {/*num_coefficients=*/2, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/4},
    {/*num_coefficients=*/3, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/2}};

PassDefinition progressive_passes_dc_lf_salient_ac[] = {
    {/*num_coefficients=*/2, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/4},
    {/*num_coefficients=*/3, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/2},
    {/*num_coefficients=*/8, /*shift=*/0, /*salient_only=*/true,
     /*suitable_for_downsampling_of_at_least=*/0}};

PassDefinition progressive_passes_dc_lf_salient_ac_other_ac[] = {
    {/*num_coefficients=*/2, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/4},
    {/*num_coefficients=*/3, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/2},
    {/*num_coefficients=*/8, /*shift=*/0, /*salient_only=*/true,
     /*suitable_for_downsampling_of_at_least=*/0},
    {/*num_coefficients=*/8, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/0}};

PassDefinition progressive_passes_dc_quant_ac_full_ac[] = {
    {/*num_coefficients=*/8, /*shift=*/2, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/4},
    {/*num_coefficients=*/8, /*shift=*/1, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/2},
    {/*num_coefficients=*/8, /*shift=*/0, /*salient_only=*/false,
     /*suitable_for_downsampling_of_at_least=*/0},
};

Status MakeImageMetadata(const CompressParams& cparams, const CodecInOut* io,
                         CodecMetadata* metadata) {
  *metadata = io->metadata;

  // Keep ICC profile in lossless modes because a reconstructed profile may be
  // slightly different (quantization).
  const bool lossless_modular =
      cparams.modular_mode && cparams.quality_pair.first == 100.0f;
  if (!lossless_modular) {
    metadata->m.color_encoding.DecideIfWantICC();
  }

  metadata->m.xyb_encoded =
      cparams.color_transform == ColorTransform::kXYB ? true : false;

  return true;
}

}  // namespace

Status EncodePreview(const CompressParams& cparams, const ImageBundle& ib,
                     const CodecMetadata* metadata, ThreadPool* pool,
                     BitWriter* JXL_RESTRICT writer) {
  BitWriter preview_writer;
  // TODO(janwas): also support generating preview by downsampling
  if (ib.HasColor()) {
    AuxOut aux_out;
    PassesEncoderState passes_enc_state;
    // TODO(lode): check if we want all extra channels and matching xyb_encoded
    // for the preview, such that using the main ImageMetadata object for
    // encoding this frame is warrented.
    FrameInfo frame_info;
    frame_info.is_preview = true;
    JXL_RETURN_IF_ERROR(EncodeFrame(cparams, frame_info, metadata, ib,
                                    &passes_enc_state, pool, &preview_writer,
                                    &aux_out));
    preview_writer.ZeroPadToByte();
  }

  if (preview_writer.BitsWritten() != 0) {
    writer->ZeroPadToByte();
    writer->AppendByteAligned(preview_writer);
  }

  return true;
}

Status WriteHeaders(const CompressParams& cparams, const CodecInOut* io,
                    CodecMetadata* metadata, BitWriter* writer,
                    AuxOut* aux_out) {
  // Marker/signature
  BitWriter::Allotment allotment(writer, 16);
  writer->Write(8, 0xFF);
  writer->Write(8, kCodestreamMarker);
  ReclaimAndCharge(writer, &allotment, kLayerHeader, aux_out);

  JXL_RETURN_IF_ERROR(MakeImageMetadata(cparams, io, metadata));

  JXL_RETURN_IF_ERROR(metadata->size.Set(io->xsize(), io->ysize()));
  JXL_RETURN_IF_ERROR(
      WriteSizeHeader(metadata->size, writer, kLayerHeader, aux_out));

  JXL_RETURN_IF_ERROR(
      WriteImageMetadata(metadata->m, writer, kLayerHeader, aux_out));

  metadata->transform_data.nonserialized_xyb_encoded =
      io->metadata.m.xyb_encoded;
  JXL_RETURN_IF_ERROR(
      Bundle::Write(metadata->transform_data, writer, kLayerHeader, aux_out));

  return true;
}

Status EncodeFile(const CompressParams& cparams, const CodecInOut* io,
                  PassesEncoderState* passes_enc_state, PaddedBytes* compressed,
                  AuxOut* aux_out, ThreadPool* pool) {
  io->CheckMetadata();
  BitWriter writer;

  CodecMetadata metadata;
  JXL_RETURN_IF_ERROR(WriteHeaders(cparams, io, &metadata, &writer, aux_out));

  // Only send ICC (at least several hundred bytes) if fields aren't enough.
  if (metadata.m.color_encoding.WantICC()) {
    JXL_RETURN_IF_ERROR(WriteICC(metadata.m.color_encoding.ICC(), &writer,
                                 kLayerHeader, aux_out));
  }

  if (metadata.m.have_preview) {
    JXL_RETURN_IF_ERROR(
        EncodePreview(cparams, io->preview_frame, &metadata, pool, &writer));
  }

  // Each frame should start on byte boundaries.
  writer.ZeroPadToByte();

  if (cparams.progressive_mode || cparams.qprogressive_mode) {
    if (cparams.saliency_map != nullptr) {
      passes_enc_state->progressive_splitter.SetSaliencyMap(
          cparams.saliency_map);
    }
    passes_enc_state->progressive_splitter.SetSaliencyThreshold(
        cparams.saliency_threshold);
    if (cparams.qprogressive_mode) {
      passes_enc_state->progressive_splitter.SetProgressiveMode(
          ProgressiveMode{progressive_passes_dc_quant_ac_full_ac});
    } else {
      switch (cparams.saliency_num_progressive_steps) {
        case 1:
          passes_enc_state->progressive_splitter.SetProgressiveMode(
              ProgressiveMode{progressive_passes_dc_vlf});
          break;
        case 2:
          passes_enc_state->progressive_splitter.SetProgressiveMode(
              ProgressiveMode{progressive_passes_dc_lf});
          break;
        case 3:
          passes_enc_state->progressive_splitter.SetProgressiveMode(
              ProgressiveMode{progressive_passes_dc_lf_salient_ac});
          break;
        case 4:
          if (cparams.saliency_threshold == 0.0f) {
            // No need for a 4th pass if saliency-threshold regards everything
            // as salient.
            passes_enc_state->progressive_splitter.SetProgressiveMode(
                ProgressiveMode{progressive_passes_dc_lf_salient_ac});
          } else {
            passes_enc_state->progressive_splitter.SetProgressiveMode(
                ProgressiveMode{progressive_passes_dc_lf_salient_ac_other_ac});
          }
          break;
        default:
          return JXL_FAILURE("Invalid saliency_num_progressive_steps.");
      }
    }
  }

  for (size_t i = 0; i < io->frames.size(); i++) {
    FrameInfo info;
    info.is_last = i == io->frames.size() - 1;
    if (io->frames[i].use_for_next_frame) {
      info.save_as_reference = 1;
    }
    JXL_RETURN_IF_ERROR(EncodeFrame(cparams, info, &metadata, io->frames[i],
                                    passes_enc_state, pool, &writer, aux_out));
  }

  // Clean up passes_enc_state in case it gets reused.
  for (size_t i = 0; i < 4; i++) {
    passes_enc_state->shared.dc_frames[i] = Image3F();
    passes_enc_state->shared.reference_frames[i].storage = ImageBundle();
  }

  *compressed = std::move(writer).TakeBytes();
  return true;
}

}  // namespace jxl