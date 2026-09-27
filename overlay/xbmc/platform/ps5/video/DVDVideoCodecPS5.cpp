/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DVDVideoCodecPS5.h"

#include "RendererPS5.h"

#include "VideoCodecRegistration.h"

#include "cores/VideoPlayer/Buffers/VideoBuffer.h"
#include "cores/VideoPlayer/DVDCodecs/DVDFactoryCodec.h"
#include "cores/VideoPlayer/DVDStreamInfo.h"
#include "cores/VideoPlayer/Interface/DemuxPacket.h"
#include "cores/VideoPlayer/Interface/TimingConstants.h"

extern "C"
{
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
}
#include "cores/VideoPlayer/Process/ProcessInfo.h"
#include "utils/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
}

using namespace KODI::PLATFORM::PS5;

namespace
{
constexpr unsigned kMaxErrorsInRow = 60; // then give up (Kodi shows an error)

bool IsH264Supported(int profile)
{
  // 8-bit 4:2:0 profiles; High 10, 4:2:2 and 4:4:4 go to FFmpeg
  switch (profile)
  {
    case AV_PROFILE_H264_BASELINE:
    case AV_PROFILE_H264_CONSTRAINED_BASELINE:
    case AV_PROFILE_H264_MAIN:
    case AV_PROFILE_H264_EXTENDED:
    case AV_PROFILE_H264_HIGH:
    case AV_PROFILE_UNKNOWN:
      return true;
    default:
      return false;
  }
}
} // namespace

CDVDVideoCodecPS5::CDVDVideoCodecPS5(CProcessInfo& processInfo) : CDVDVideoCodec(processInfo)
{
}

CDVDVideoCodecPS5::~CDVDVideoCodecPS5()
{
  ClearQueue();
  CloseDeinterlacer();
  av_bsf_free(&m_bsf);
  av_packet_free(&m_packet);
  // no Close(): zero-copy pictures may still show the decoder's frames; the
  // last owner of m_decoder closes it
  m_decoder.reset();
}

std::unique_ptr<CDVDVideoCodec> CDVDVideoCodecPS5::Create(CProcessInfo& processInfo)
{
  return std::make_unique<CDVDVideoCodecPS5>(processInfo);
}

void CDVDVideoCodecPS5::Register()
{
  CDVDFactoryCodec::RegisterHWVideoCodec("ps5-videodec2", &CDVDVideoCodecPS5::Create);
}

bool CDVDVideoCodecPS5::Open(CDVDStreamInfo& hints, CDVDCodecOptions& options)
{
  if (getenv("KODI_PS5_SWDECODE") != nullptr)
  {
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: kodi-swdecode present, using software decoding");
    return false;
  }

  VideoDec2Codec codec;
  if (hints.codec == AV_CODEC_ID_H264 && IsH264Supported(hints.profile))
    codec = VideoDec2Codec::H264;
  else if (hints.codec == AV_CODEC_ID_HEVC &&
           (hints.profile == AV_PROFILE_HEVC_MAIN || hints.profile == AV_PROFILE_UNKNOWN) &&
           hints.bitsperpixel <= 8)
    codec = VideoDec2Codec::HEVC;
  else if (hints.codec == AV_CODEC_ID_HEVC &&
           (hints.profile == AV_PROFILE_HEVC_MAIN_10 ||
            (hints.profile == AV_PROFILE_UNKNOWN && hints.bitsperpixel == 10)))
    codec = VideoDec2Codec::HEVCMain10;
  // VP9 (kodi-hw-vp9, until confirmed on the console)
  else if (hints.codec == AV_CODEC_ID_VP9 && getenv("KODI_PS5_HW_VP9") &&
           (hints.profile == AV_PROFILE_VP9_0 ||
            (hints.profile == AV_PROFILE_UNKNOWN && hints.bitsperpixel <= 8)))
    codec = VideoDec2Codec::VP9;
  else if (hints.codec == AV_CODEC_ID_VP9 && getenv("KODI_PS5_HW_VP9") &&
           (hints.profile == AV_PROFILE_VP9_2 ||
            (hints.profile == AV_PROFILE_UNKNOWN && hints.bitsperpixel == 10)))
    codec = VideoDec2Codec::VP9Profile2;
  else
    return false; // FFmpeg takes it (H.264 High 10, HEVC/VP9 4:2:2/4:4:4, 12-bit, AV1, ...)

  if (hints.width <= 0 || hints.height <= 0 || hints.width > 3840 || hints.height > 2176)
    return false;
  // Interlaced streams go to FFmpeg (which deinterlaces), unless the
  // kodi-hw-interlaced probe sends them to the hardware decoder.
  const bool interlaced = hints.interlaced;
  if (interlaced && !getenv("KODI_PS5_HW_INTERLACED"))
    return false;
  if (interlaced)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: kodi-hw-interlaced: interlaced {}x{} stream to the "
              "hardware decoder (no deinterlacing yet)", hints.width, hints.height);

  // interlaced (kodi-hw-interlaced): bwdif over the decoded frames, which
  // needs them in ordinary memory - so no zero-copy for these
  m_deinterlace = interlaced;
  // zero-copy (kodi-zerocopy): pictures are the decoder's own frames
  m_zeroCopy = !interlaced && getenv("KODI_PS5_ZEROCOPY") != nullptr && IsZeroCopyAvailable();
  if (getenv("KODI_PS5_ZEROCOPY") && !m_zeroCopy)
    CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: kodi-zerocopy present, but the GL driver has no "
              "zero-copy additions (rebuild it with scripts/18): copying frames");
  m_decoder->SetPooled(m_zeroCopy);
  if (m_zeroCopy && !m_zeroCopyPool)
    m_zeroCopyPool = std::make_shared<CVideoBufferPoolPS5>();

  std::string error;
  if (!m_decoder->Open(codec, hints.width, hints.height, error, interlaced))
  {
    CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: hardware decoder unavailable ({}), using FFmpeg",
              error);
    m_decoder->Close();
    return false;
  }
  if (!SetupBitstreamFilter(hints))
  {
    m_decoder->Close();
    return false;
  }

  m_width = static_cast<unsigned>(hints.width);
  m_height = static_cast<unsigned>(hints.height);
  m_displayWidth = m_width;
  m_displayHeight = m_height;
  if (hints.aspect > 0.0)
  {
    m_displayWidth = static_cast<unsigned>(std::lrint(m_height * hints.aspect)) & ~3u;
    if (m_displayWidth < m_width)
    {
      m_displayWidth = m_width;
      m_displayHeight = static_cast<unsigned>(std::lrint(m_width / hints.aspect)) & ~3u;
    }
  }
  m_frameDuration = (hints.fpsrate > 0 && hints.fpsscale > 0)
                        ? DVD_TIME_BASE * static_cast<double>(hints.fpsscale) / hints.fpsrate
                        : DVD_TIME_BASE / 25.0;
  m_colorSpace = hints.colorSpace;
  m_colorPrimaries = hints.colorPrimaries;
  m_colorTransfer = hints.colorTransferCharacteristic;
  m_fullRange = hints.colorRange == AVCOL_RANGE_JPEG;
  m_stereoMode = hints.stereo_mode;

  m_vp9 = codec == VideoDec2Codec::VP9 || codec == VideoDec2Codec::VP9Profile2;
  m_tenBit = codec == VideoDec2Codec::HEVCMain10 || codec == VideoDec2Codec::VP9Profile2;
  m_alignmentKnown = !m_tenBit;
  m_pixelFormat = m_tenBit ? AV_PIX_FMT_YUV420P16 : AV_PIX_FMT_NV12;
  m_colorBits = m_tenBit ? 16 : 8;
  // HDR10 metadata, for Kodi's tone mapping on an SDR output
  m_hasDisplayMetadata = hints.masteringMetadata != nullptr;
  if (m_hasDisplayMetadata)
    m_displayMetadata = *hints.masteringMetadata;
  m_hasLightMetadata = hints.contentLightMetadata != nullptr;
  if (m_hasLightMetadata)
    m_lightMetadata = *hints.contentLightMetadata;

  m_processInfo.SetVideoDecoderName(GetName(), true);
  m_processInfo.SetVideoPixelFormat(m_tenBit ? "p010" : "nv12");
  if (m_vp9)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: VP9: superframes split, hidden frames not shown");
  if (m_zeroCopy)
    CLog::Log(LOGINFO, "CDVDVideoCodecPS5: zero-copy: pictures are the decoder's own frames");
  m_processInfo.SetVideoDimensions(hints.width, hints.height);
  m_processInfo.SetVideoDeintMethod("none");
  if (hints.fpsrate > 0 && hints.fpsscale > 0)
    m_processInfo.SetVideoFps(static_cast<float>(hints.fpsrate) / hints.fpsscale);

  CLog::Log(LOGINFO, "CDVDVideoCodecPS5: hardware {} decoding {}x{}",
            codec == VideoDec2Codec::H264
                ? "H.264"
                : (m_vp9 ? (m_tenBit ? "VP9 Profile 2" : "VP9")
                         : (m_tenBit ? "HEVC Main10" : "HEVC")),
            hints.width, hints.height);
  return true;
}

bool CDVDVideoCodecPS5::SetupBitstreamFilter(const CDVDStreamInfo& hints)
{
  m_packet = av_packet_alloc();
  if (!m_packet)
    return false;

  if (hints.codec == AV_CODEC_ID_VP9)
  {
    // the decoder refuses a compound superframe but takes each of its frames
    const AVBitStreamFilter* split = av_bsf_get_by_name("vp9_superframe_split");
    if (!split || av_bsf_alloc(split, &m_bsf) < 0)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: bitstream filter vp9_superframe_split unavailable");
      return false;
    }
    m_bsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
    m_bsf->par_in->codec_id = AV_CODEC_ID_VP9;
    if (av_bsf_init(m_bsf) < 0)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: cannot initialise vp9_superframe_split");
      av_bsf_free(&m_bsf);
      return false;
    }
    return true;
  }

  // MP4/MKV carry avcC/hvcC (length-prefixed NAL units, parameter sets in
  // extradata); the decoder wants Annex-B. Already Annex-B: no filter.
  const uint8_t* extra = hints.extradata.GetData();
  const size_t extraSize = hints.extradata.GetSize();
  if (!extra || extraSize < 4 || extra[0] != 1)
    return true;

  const char* name = hints.codec == AV_CODEC_ID_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb";
  const AVBitStreamFilter* filter = av_bsf_get_by_name(name);
  if (!filter || av_bsf_alloc(filter, &m_bsf) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: bitstream filter {} unavailable", name);
    return false;
  }
  m_bsf->par_in->codec_type = AVMEDIA_TYPE_VIDEO;
  m_bsf->par_in->codec_id = hints.codec;
  m_bsf->par_in->extradata =
      static_cast<uint8_t*>(av_mallocz(extraSize + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!m_bsf->par_in->extradata)
    return false;
  std::memcpy(m_bsf->par_in->extradata, extra, extraSize);
  m_bsf->par_in->extradata_size = static_cast<int>(extraSize);
  if (av_bsf_init(m_bsf) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: cannot initialise {}", name);
    av_bsf_free(&m_bsf);
    return false;
  }
  return true;
}

bool CDVDVideoCodecPS5::AddData(const DemuxPacket& packet)
{
  if (m_fatal)
    return false;
  if (!packet.pData || packet.iSize <= 0)
    return true;
  // one decode per packet may leave a picture queued; take it first
  if (m_decoded.size() >= 2)
    return false;
  // zero-copy: every frame shown or held - Kodi releases pictures first
  if (!RetryPending() || !m_decoder->HasFreeFrame())
    return false;

  const double pts = packet.pts != DVD_NOPTS_VALUE ? packet.pts : packet.dts;
  if (pts != DVD_NOPTS_VALUE)
    m_pts.insert(pts);

  if (!m_bsf)
    return DecodeOne(packet.pData, static_cast<size_t>(packet.iSize)) || !m_fatal;

  av_packet_unref(m_packet);
  if (av_new_packet(m_packet, packet.iSize) < 0)
    return true;
  std::memcpy(m_packet->data, packet.pData, static_cast<size_t>(packet.iSize));
  if (av_bsf_send_packet(m_bsf, m_packet) < 0)
    return true; // bad packet: drop it
  while (av_bsf_receive_packet(m_bsf, m_packet) == 0)
  {
    // once one access unit waits for a frame, the following ones queue behind it
    if (!m_pendingAus.empty())
      m_pendingAus.emplace_back(m_packet->data, m_packet->data + m_packet->size);
    else
      DecodeOne(m_packet->data, static_cast<size_t>(m_packet->size));
    av_packet_unref(m_packet);
  }
  return !m_fatal;
}

namespace
{
// VP9 uncompressed header, first bits: frame_marker(2), profile(2, +1 reserved
// for profile 3), show_existing_frame(1), then frame_type(1), show_frame(1).
// Hidden frames (alternate references) still produce a decoder output, which
// must not be shown; a show-existing-frame command is shown.
bool Vp9FrameIsShown(const uint8_t* data, size_t size)
{
  if (size < 1)
    return true;
  unsigned bit = 0;
  auto read = [&](unsigned count)
  {
    unsigned value = 0;
    for (unsigned i = 0; i < count; ++i, ++bit)
    {
      if (bit / 8 >= size)
        return value << (count - i); // truncated: treat as shown further up
      value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1u);
    }
    return value;
  };
  if (read(2) != 2)
    return true; // not a VP9 frame header: never hide on a guess
  const unsigned profile = read(1) | (read(1) << 1);
  if (profile == 3)
    read(1);
  if (read(1)) // show_existing_frame
    return true;
  read(1); // frame_type
  return read(1) != 0; // show_frame
}
} // namespace

bool CDVDVideoCodecPS5::DecodeOne(const uint8_t* data, size_t size)
{
  const bool shown = !m_vp9 || Vp9FrameIsShown(data, size);
  bool gotPicture = false;
  VideoDec2Picture picture;
  std::string error;
  if (!m_decoder->Decode(data, size, gotPicture, &picture, error))
  {
    if (m_errorsInRow++ < 5)
      CLog::Log(LOGWARNING, "CDVDVideoCodecPS5: {}", error);
    if (m_errorsInRow >= kMaxErrorsInRow)
    {
      CLog::Log(LOGERROR, "CDVDVideoCodecPS5: {} decode errors in a row, giving up",
                m_errorsInRow);
      m_fatal = true;
    }
    return false;
  }
  m_errorsInRow = 0;
  if (m_decoder->Stalled())
  {
    // zero-copy: no frame was free; the access unit waits at the front (it is
    // either the first waiting one or a retried one)
    m_pendingAus.emplace_front(data, data + size);
    return true;
  }
  if (gotPicture && !shown)
  {
    // VP9 hidden frame: decoded as a reference, never presented
    m_decoder->ReleaseFrame(picture.frameIndex);
    return true;
  }
  if (gotPicture)
    Keep(picture);
  return true;
}

bool CDVDVideoCodecPS5::RetryPending()
{
  while (!m_pendingAus.empty())
  {
    if (!m_decoder->HasFreeFrame())
      return false;
    std::vector<uint8_t> au = std::move(m_pendingAus.front());
    m_pendingAus.pop_front();
    DecodeOne(au.data(), au.size());
    if (!m_pendingAus.empty() && m_decoder->Stalled())
      return false; // it went back to the front: still no frame
  }
  return true;
}

double CDVDVideoCodecPS5::NextPts()
{
  if (!m_pts.empty())
  {
    m_lastPts = *m_pts.begin();
    m_pts.erase(m_pts.begin());
  }
  else
    m_lastPts += m_frameDuration;
  return m_lastPts;
}

void CDVDVideoCodecPS5::DetectAlignment(const VideoDec2Picture& picture, unsigned width,
                                        unsigned height)
{
  // Sample the luma plane: P010 keeps the value in the upper 10 bits (the
  // low 6 always zero), the other layout in the lower 10 (the top 6 zero).
  uint16_t lowBits = 0, highBits = 0;
  for (unsigned y = 0; y < height; y += std::max(1u, height / 64))
  {
    const auto* row = reinterpret_cast<const uint16_t*>(picture.data + y * picture.pitch);
    for (unsigned x = 0; x < width; x += std::max(1u, width / 64))
    {
      lowBits |= row[x] & 0x003f;
      highBits |= row[x] & 0xfc00;
    }
  }
  const bool msb = lowBits == 0 || highBits != 0;
  m_pixelFormat = msb ? AV_PIX_FMT_YUV420P16 : AV_PIX_FMT_YUV420P10;
  m_colorBits = msb ? 16 : 10;
  m_alignmentKnown = true;
  CLog::Log(LOGINFO,
            "CDVDVideoCodecPS5: 10-bit samples {} (low bits {:#x}, high bits {:#x}): passed "
            "as {}",
            msb ? "in the upper 10 bits (P010)" : "in the lower 10 bits", lowBits, highBits,
            msb ? "yuv420p16" : "yuv420p10");
  m_processInfo.SetVideoPixelFormat(msb ? "p010" : "p010 (lsb)");
}

bool CDVDVideoCodecPS5::Keep(const VideoDec2Picture& picture)
{
  // Visible area: the stream's size, never more than what was decoded.
  const unsigned width = std::min(m_width, picture.width);
  const unsigned height = std::min(m_height, picture.height);
  const size_t pitch = picture.pitch;
  const size_t size = pitch * height * 3 / 2;
  if (m_tenBit && !m_alignmentKnown)
    DetectAlignment(picture, width, height);

  if (m_deinterlace)
    return KeepDeinterlaced(picture, width, height);

  if (m_zeroCopy)
  {
    // the picture is the decoder's frame itself; CRendererPS5 shows it
    auto* frame = static_cast<CVideoBufferPS5*>(m_zeroCopyPool->Get());
    frame->Set(m_decoder, picture, m_tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12);
    m_decoded.push_back(Decoded{frame, NextPts()});
    return true;
  }

  CVideoBuffer* buffer =
      m_processInfo.GetVideoBufferManager().Get(m_pixelFormat, static_cast<int>(size), nullptr);
  if (!buffer)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: no video buffer available");
    NextPts(); // keep timestamps in step
    return false;
  }
  uint8_t* planes[YuvImage::MAX_PLANES];
  const uint8_t* chroma = picture.data + pitch * picture.height; // after the full coded height
  if (!m_tenBit)
  {
    const int strides[YuvImage::MAX_PLANES] = {static_cast<int>(pitch), static_cast<int>(pitch),
                                               0};
    buffer->SetDimensions(static_cast<int>(width), static_cast<int>(height), strides);
    buffer->GetPlanes(planes);
    // luma: `height` visible rows; interleaved chroma
    std::memcpy(planes[0], picture.data, pitch * height);
    std::memcpy(planes[1], chroma, pitch * height / 2);
  }
  else
  {
    // 16-bit planar for Kodi's renderer: luma copied as is, the interleaved
    // U/V samples split into two planes of half the row length.
    const int strides[YuvImage::MAX_PLANES] = {static_cast<int>(pitch),
                                               static_cast<int>(pitch / 2),
                                               static_cast<int>(pitch / 2)};
    buffer->SetDimensions(static_cast<int>(width), static_cast<int>(height), strides);
    buffer->GetPlanes(planes);
    std::memcpy(planes[0], picture.data, pitch * height);
    const unsigned chromaWidth = (width + 1) / 2;
    for (unsigned y = 0; y < (height + 1) / 2; ++y)
    {
      const auto* src = reinterpret_cast<const uint16_t*>(chroma + y * pitch);
      auto* u = reinterpret_cast<uint16_t*>(planes[1] + y * (pitch / 2));
      auto* v = reinterpret_cast<uint16_t*>(planes[2] + y * (pitch / 2));
      for (unsigned x = 0; x < chromaWidth; ++x)
      {
        u[x] = src[2 * x];
        v[x] = src[2 * x + 1];
      }
    }
  }

  m_decoded.push_back(Decoded{buffer, NextPts()});
  return true;
}

bool CDVDVideoCodecPS5::SetupDeinterlacer()
{
  // NV12 from the decoder is split into planar 4:2:0 on the way in (bwdif
  // works on planar formats); fields in top-field-first order, as broadcast
  m_deintGraph = avfilter_graph_alloc();
  m_deintIn = av_frame_alloc();
  m_deintOut = av_frame_alloc();
  if (!m_deintGraph || !m_deintIn || !m_deintOut)
    return false;
  char args[256];
  snprintf(args, sizeof(args),
           "video_size=%ux%u:pix_fmt=%d:time_base=1/%d:pixel_aspect=1/1", m_width, m_height,
           static_cast<int>(AV_PIX_FMT_YUV420P), static_cast<int>(DVD_TIME_BASE));
  const AVFilter* bwdif = avfilter_get_by_name("bwdif");
  AVFilterContext* deint = nullptr;
  if (!bwdif ||
      avfilter_graph_create_filter(&m_deintSource, avfilter_get_by_name("buffer"), "in", args,
                                   nullptr, m_deintGraph) < 0 ||
      avfilter_graph_create_filter(&deint, bwdif, "deint", "mode=send_field:parity=tff:deint=all",
                                   nullptr, m_deintGraph) < 0 ||
      avfilter_graph_create_filter(&m_deintSink, avfilter_get_by_name("buffersink"), "out",
                                   nullptr, nullptr, m_deintGraph) < 0 ||
      avfilter_link(m_deintSource, 0, deint, 0) < 0 || avfilter_link(deint, 0, m_deintSink, 0) < 0 ||
      avfilter_graph_config(m_deintGraph, nullptr) < 0)
  {
    CLog::Log(LOGERROR, "CDVDVideoCodecPS5: the bwdif deinterlacer could not be set up");
    return false;
  }
  m_deintIn->format = AV_PIX_FMT_YUV420P;
  m_deintIn->width = static_cast<int>(m_width);
  m_deintIn->height = static_cast<int>(m_height);
  if (av_frame_get_buffer(m_deintIn, 64) < 0)
    return false;
  CLog::Log(LOGINFO, "CDVDVideoCodecPS5: deinterlacing {}x{} with bwdif (one picture per field)",
            m_width, m_height);
  m_processInfo.SetVideoDeintMethod("bwdif");
  return true;
}

void CDVDVideoCodecPS5::CloseDeinterlacer()
{
  avfilter_graph_free(&m_deintGraph); // frees the filter contexts too
  m_deintSource = nullptr;
  m_deintSink = nullptr;
  av_frame_free(&m_deintIn);
  av_frame_free(&m_deintOut);
}

bool CDVDVideoCodecPS5::KeepDeinterlaced(const VideoDec2Picture& picture, unsigned width,
                                         unsigned height)
{
  if (!m_deintGraph && !SetupDeinterlacer())
  {
    CloseDeinterlacer();
    m_deinterlace = false; // show the woven frames rather than nothing
    return Keep(picture);
  }
  if (av_frame_make_writable(m_deintIn) < 0)
    return false;
  // luma rows as they are; interleaved chroma split into U and V
  for (unsigned y = 0; y < height; ++y)
    std::memcpy(m_deintIn->data[0] + static_cast<size_t>(y) * m_deintIn->linesize[0],
                picture.data + static_cast<size_t>(y) * picture.pitch, width);
  const uint8_t* chroma = picture.data + static_cast<size_t>(picture.pitch) * picture.height;
  for (unsigned y = 0; y < height / 2; ++y)
  {
    const uint8_t* src = chroma + static_cast<size_t>(y) * picture.pitch;
    uint8_t* u = m_deintIn->data[1] + static_cast<size_t>(y) * m_deintIn->linesize[1];
    uint8_t* v = m_deintIn->data[2] + static_cast<size_t>(y) * m_deintIn->linesize[2];
    for (unsigned x = 0; x < width / 2; ++x)
    {
      u[x] = src[2 * x];
      v[x] = src[2 * x + 1];
    }
  }
  m_deintIn->pts = m_deintFrames++;
  m_deintIn->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
  m_deintPts.push_back(NextPts());
  if (av_buffersrc_add_frame_flags(m_deintSource, m_deintIn, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
    return false;

  while (av_buffersink_get_frame(m_deintSink, m_deintOut) >= 0)
  {
    // the two fields of the front input: its pts, then half a frame later
    double pts = DVD_NOPTS_VALUE;
    if (!m_deintPts.empty())
    {
      pts = m_deintPts.front() + (m_deintOutputs == 1 ? m_frameDuration / 2 : 0.0);
      if (++m_deintOutputs == 2)
      {
        m_deintPts.pop_front();
        m_deintOutputs = 0;
      }
    }
    const int strides[YuvImage::MAX_PLANES] = {static_cast<int>(width),
                                               static_cast<int>(width / 2),
                                               static_cast<int>(width / 2)};
    const size_t size = static_cast<size_t>(width) * height * 3 / 2;
    CVideoBuffer* buffer = m_processInfo.GetVideoBufferManager().Get(AV_PIX_FMT_YUV420P,
                                                                     static_cast<int>(size), nullptr);
    if (buffer)
    {
      buffer->SetDimensions(static_cast<int>(width), static_cast<int>(height), strides);
      uint8_t* planes[YuvImage::MAX_PLANES];
      buffer->GetPlanes(planes);
      for (int p = 0; p < 3; ++p)
      {
        const unsigned rows = p == 0 ? height : height / 2;
        const unsigned bytes = p == 0 ? width : width / 2;
        for (unsigned y = 0; y < rows; ++y)
          std::memcpy(planes[p] + static_cast<size_t>(y) * strides[p],
                      m_deintOut->data[p] + static_cast<size_t>(y) * m_deintOut->linesize[p],
                      bytes);
      }
      m_decoded.push_back(Decoded{buffer, pts});
    }
    av_frame_unref(m_deintOut);
  }
  return true;
}

void CDVDVideoCodecPS5::ClearQueue()
{
  for (auto& d : m_decoded)
    if (d.buffer)
      d.buffer->Release();
  m_decoded.clear();
}

void CDVDVideoCodecPS5::Reset()
{
  ClearQueue();
  m_pts.clear();
  m_decoder->Reset();
  m_pendingAus.clear();
  CloseDeinterlacer(); // set up again with the next picture
  m_deintPts.clear();
  m_deintOutputs = 0;
  if (m_bsf)
    av_bsf_flush(m_bsf);
  m_errorsInRow = 0;
  m_codecControlFlags = 0;
}

CDVDVideoCodec::VCReturn CDVDVideoCodecPS5::GetPicture(VideoPicture* pVideoPicture)
{
  if (m_fatal)
    return VC_ERROR;

  if (m_decoded.empty() && (m_codecControlFlags & DVD_CODEC_CTRL_DRAIN))
  {
    VideoDec2Picture picture;
    if (m_decoder->Flush(&picture))
      Keep(picture);
    else
      return VC_EOF;
  }
  if (m_decoded.empty())
    return VC_BUFFER;

  Decoded d = m_decoded.front();
  m_decoded.pop_front();

  pVideoPicture->Reset(); // releases the previous picture's buffer
  pVideoPicture->videoBuffer = d.buffer;
  pVideoPicture->pts = d.pts;
  pVideoPicture->dts = DVD_NOPTS_VALUE;
  pVideoPicture->iDuration = m_deinterlace ? m_frameDuration / 2 : m_frameDuration;
  pVideoPicture->iWidth = m_width;
  pVideoPicture->iHeight = m_height;
  pVideoPicture->iDisplayWidth = m_displayWidth;
  pVideoPicture->iDisplayHeight = m_displayHeight;
  pVideoPicture->pixelFormat =
      m_deinterlace ? AV_PIX_FMT_YUV420P
                    : (m_zeroCopy ? (m_tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12) : m_pixelFormat);
  pVideoPicture->colorBits = m_deinterlace ? 8 : m_colorBits;
  pVideoPicture->hasDisplayMetadata = m_hasDisplayMetadata;
  if (m_hasDisplayMetadata)
    pVideoPicture->displayMetadata = m_displayMetadata;
  pVideoPicture->hasLightMetadata = m_hasLightMetadata;
  if (m_hasLightMetadata)
    pVideoPicture->lightMetadata = m_lightMetadata;
  pVideoPicture->color_space = m_colorSpace;
  pVideoPicture->color_primaries = m_colorPrimaries;
  pVideoPicture->m_originalColorPrimaries = m_colorPrimaries;
  pVideoPicture->color_transfer = m_colorTransfer;
  pVideoPicture->color_range = m_fullRange ? 1 : 0;
  pVideoPicture->stereoMode = m_stereoMode;
  if (m_codecControlFlags & DVD_CODEC_CTRL_DROP)
    pVideoPicture->iFlags |= DVP_FLAG_DROPPED;
  return VC_PICTURE;
}

void KODI::PLATFORM::PS5::RegisterVideoCodecs()
{
  CDVDVideoCodecPS5::Register();
  // the zero-copy renderer takes only the decoder's own frames (kodi-zerocopy)
  CRendererPS5::Register();
}
