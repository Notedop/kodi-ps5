/*
 *  Copyright (C) 2026 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "VideoBufferPS5.h"
#include "VideoDec2.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodec.h"

#include <deque>
#include <cstdlib>
#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct AVBSFContext;
struct AVPacket;
class CVideoBuffer;

extern "C"
{
struct AVFilterGraph;
struct AVFilterContext;
struct AVFrame;
}

namespace KODI::PLATFORM::PS5
{

/*!
 * Hardware H.264 / HEVC Main decoding on the PS5 (libSceVideodec2).
 *
 * Pictures are copied out of the decoder's frame buffers into Kodi's NV12
 * system-memory buffers, which the OpenGL renderer uploads as usual. Streams
 * the decoder cannot take (other codecs, 10-bit, 4:2:2 ...) fall back to FFmpeg.
 * An empty file "kodi-swdecode" in the title folder turns this decoder off.
 */
class CDVDVideoCodecPS5 : public CDVDVideoCodec
{
public:
  explicit CDVDVideoCodecPS5(CProcessInfo& processInfo);
  ~CDVDVideoCodecPS5() override;

  static std::unique_ptr<CDVDVideoCodec> Create(CProcessInfo& processInfo);
  static void Register();

  bool Open(CDVDStreamInfo& hints, CDVDCodecOptions& options) override;
  bool AddData(const DemuxPacket& packet) override;
  void Reset() override;
  VCReturn GetPicture(VideoPicture* pVideoPicture) override;
  const char* GetName() override { return "ps5-videodec2"; }
  unsigned GetAllowedReferences() override { return 4; }
  void SetCodecControl(int flags) override { m_codecControlFlags = flags; }

private:
  struct Decoded
  {
    CVideoBuffer* buffer = nullptr;
    double pts = 0;
  };

  bool SetupBitstreamFilter(const CDVDStreamInfo& hints);
  bool DecodeOne(const uint8_t* data, size_t size);
  bool Keep(const VideoDec2Picture& picture);
  // A returned picture may still be being written when the pipeline is deeper
  // than 1; it is complete once `depth-1` later pictures have come back. So
  // pictures wait here that long before Keep() (drained at end of stream).
  bool Accept(const VideoDec2Picture& picture);
  // kodi-debug trace of the first pictures after an open or a reset: frames
  // offered/accepted/returned, and luma samples of the returned picture at
  // return and at hand-off (black regions read 0 or 16)
  unsigned m_trace = 0;
  std::string LumaSamples(const VideoDec2Picture& picture) const;
  void Trace(const char* stage, const VideoDec2Picture& picture, bool gotPicture);
  void ReleaseHeld();
  std::deque<VideoDec2Picture> m_held;
  unsigned m_hold = 0;
  double NextPts();
  void ClearQueue();

  // shared with zero-copy pictures: the decoder's memory lives until the last
  // picture showing one of its frames is released
  std::shared_ptr<KODI::PLATFORM::PS5::CVideoDec2> m_decoder =
      std::make_shared<KODI::PLATFORM::PS5::CVideoDec2>();
  bool m_zeroCopy = false; // kodi-zerocopy, with the GL driver additions present
  std::shared_ptr<KODI::PLATFORM::PS5::CVideoBufferPoolPS5> m_zeroCopyPool;
  // zero-copy: access units waiting for a free frame, in stream order
  std::deque<std::vector<uint8_t>> m_pendingAus;
  bool RetryPending();
  AVBSFContext* m_bsf = nullptr;
  AVPacket* m_packet = nullptr;

  std::deque<Decoded> m_decoded;
  std::multiset<double> m_pts; // decoded in display order: the smallest pending pts is next
  bool m_ptsTrimmed = false;   // logged once: stale timestamps discarded
  double m_lastPts = 0;
  double m_frameDuration = 0;

  unsigned m_width = 0;
  unsigned m_height = 0;
  unsigned m_displayWidth = 0;
  unsigned m_displayHeight = 0;
  CDVDStreamInfo* m_hints = nullptr;
  AVColorSpace m_colorSpace = AVCOL_SPC_UNSPECIFIED;
  AVColorPrimaries m_colorPrimaries = AVCOL_PRI_UNSPECIFIED;
  AVColorTransferCharacteristic m_colorTransfer = AVCOL_TRC_UNSPECIFIED;

  // 10-bit HEVC: the decoder's samples are 16 bits wide, the value either in
  // the upper 10 bits (P010) or the lower 10; decided on the first picture.
  bool m_tenBit = false;
  bool m_vp9 = false; // superframes split; hidden frames' outputs not shown
  bool m_hevc = false;
  // HEVC after a seek (or at the start): the demuxer resumes at a CRA picture
  // whose RASL leading pictures reference frames from before it. FFmpeg drops
  // them silently; the hardware decoder errors on each. Dropped until the
  // first non-RASL picture.
  bool m_skipRasl = false;
  static bool HevcAccessUnitIsRasl(const uint8_t* data, size_t size);
  // VP9: show flags of access units whose picture has not come out yet (with
  // frames in flight, a picture belongs to an earlier access unit)
  std::deque<bool> m_vp9PendingShown;

  // decode time statistics: every 5 seconds with kodi-debug, and one summary
  // line per stream when the codec closes (always)
  const bool m_timeDecodes = getenv("KODI_PS5_DEBUG") != nullptr;
  std::string m_streamName;      // for the summary: codec and size
  float m_streamFps = 0.0f;
  std::chrono::steady_clock::time_point m_streamStart{};
  unsigned m_streamDecodes = 0;  // pictures decoded in this stream
  double m_streamDecodeMs = 0.0; // total decode time
  double m_streamMaxMs = 0.0;
  unsigned m_streamOverBudget = 0; // decodes longer than one frame period
  void LogStreamSummary();
  std::chrono::steady_clock::time_point m_decodeWindow{};
  double m_decodeTotalMs = 0.0;
  double m_decodeMaxMs = 0.0;
  unsigned m_decodeCount = 0;
  bool m_alignmentKnown = false;
  unsigned m_alignmentSamples = 0; // pictures sampled without a decision
  AVPixelFormat m_pixelFormat = AV_PIX_FMT_NV12;
  unsigned m_colorBits = 8;
  bool m_hasDisplayMetadata = false;
  AVMasteringDisplayMetadata m_displayMetadata{};
  bool m_hasLightMetadata = false;
  AVContentLightMetadata m_lightMetadata{};

  void DetectAlignment(const VideoDec2Picture& picture, unsigned width, unsigned height);

  // kodi-hw-interlaced: FFmpeg's bwdif over the hardware decoder's (woven)
  // frames, one progressive picture per field
  bool m_deinterlace = false;
  AVFilterGraph* m_deintGraph = nullptr;
  AVFilterContext* m_deintSource = nullptr;
  AVFilterContext* m_deintSink = nullptr;
  AVFrame* m_deintIn = nullptr;
  AVFrame* m_deintOut = nullptr;
  int64_t m_deintFrames = 0;
  std::deque<double> m_deintPts; // input pictures' pts, for the two fields each
  unsigned m_deintOutputs = 0;   // outputs of the front input so far
  bool SetupDeinterlacer();
  void CloseDeinterlacer();
  bool KeepDeinterlaced(const VideoDec2Picture& picture, unsigned width, unsigned height);
  bool m_fullRange = false;
  std::string m_stereoMode;

  int m_codecControlFlags = 0;
  unsigned m_errorsInRow = 0;
  bool m_fatal = false;
};

} // namespace KODI::PLATFORM::PS5
