// Copyright (c) 2009 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SPDY_SPDY_FRAMER_H_
#define NET_SPDY_SPDY_FRAMER_H_

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif
#include <map>
#include <string>

#include "base/basictypes.h"
#include "base/logging.h"
#include "base/scoped_ptr.h"
#include "net/spdy/spdy_protocol.h"
#include "testing/gtest/include/gtest/gtest_prod.h"

typedef struct z_stream_s z_stream;  // Forward declaration for zlib.

namespace net {
class HttpNetworkLayer;
class SpdyNetworkTransactionTest;
}

namespace spdy {

class SpdyFramer;
class SpdyFramerTest;

namespace test {
class TestSpdyVisitor;
void FramerSetEnableCompressionHelper(SpdyFramer* framer, bool compress);
}  // namespace test

// A datastructure for holding a set of headers from either a
// SYN_STREAM or SYN_REPLY frame.
typedef std::map<std::string, std::string> SpdyHeaderBlock;

// SpdyFramerVisitorInterface is a set of callbacks for the SpdyFramer.
// Implement this interface to receive event callbacks as frames are
// decoded from the framer.
class SpdyFramerVisitorInterface {
 public:
  virtual ~SpdyFramerVisitorInterface() {}

  // Called if an error is detected in the SpdyFrame protocol.
  virtual void OnError(SpdyFramer* framer) = 0;

  // Called when a Control Frame is received.
  virtual void OnControl(const SpdyControlFrame* frame) = 0;

  // Called when data is received.
  // |stream_id| The stream receiving data.
  // |data| A buffer containing the data received.
  // |len| The length of the data buffer.
  // When the other side has finished sending data on this stream,
  // this method will be called with a zero-length buffer.
  virtual void OnStreamFrameData(SpdyStreamId stream_id,
                                 const char* data,
                                 size_t len) = 0;
};

class SpdyFramer {
 public:
  // SPDY states.
  // TODO(mbelshe): Can we move these into the implementation
  //                and avoid exposing through the header.  (Needed for test)
  enum SpdyState {
    SPDY_ERROR,
    SPDY_DONE,
    SPDY_RESET,
    SPDY_AUTO_RESET,
    SPDY_READING_COMMON_HEADER,
    SPDY_INTERPRET_CONTROL_FRAME_COMMON_HEADER,
    SPDY_CONTROL_FRAME_PAYLOAD,
    SPDY_IGNORE_REMAINING_PAYLOAD,
    SPDY_FORWARD_STREAM_FRAME
  };

  // SPDY error codes.
  enum SpdyError {
    SPDY_NO_ERROR,
    SPDY_INVALID_CONTROL_FRAME,      // Control frame is mal-formatted.
    SPDY_CONTROL_PAYLOAD_TOO_LARGE,  // Control frame payload was too large.
    SPDY_ZLIB_INIT_FAILURE,          // The Zlib library could not initialize.
    SPDY_UNSUPPORTED_VERSION,        // Control frame has unsupported version.
    SPDY_DECOMPRESS_FAILURE,         // There was an error decompressing.
    SPDY_COMPRESS_FAILURE,           // There was an error compressing.

    LAST_ERROR,  // Must be the last entry in the enum.
  };

  // Create a new Framer.
  SpdyFramer();
  virtual ~SpdyFramer();

  // Set callbacks to be called from the framer.  A visitor must be set, or
  // else the framer will likely crash.  It is acceptable for the visitor
  // to do nothing.  If this is called multiple times, only the last visitor
  // will be used.
  void set_visitor(SpdyFramerVisitorInterface* visitor) {
    visitor_ = visitor;
  }

  size_t remaining_payload() {
    return remaining_payload_;
  }

  // Pass data into the framer for parsing.
  // Returns the number of bytes consumed. It is safe to pass more bytes in
  // than may be consumed.
  size_t ProcessInput(const char* data, size_t len);

  // Resets the framer state after a frame has been successfully decoded.
  // TODO(mbelshe): can we make this private?
  void Reset();

  // Check the state of the framer.
  SpdyError error_code() const { return error_code_; }
  SpdyState state() const { return state_; }

  bool MessageFullyRead() {
    return state_ == SPDY_DONE || state_ == SPDY_AUTO_RESET;
  }
  bool HasError() { return state_ == SPDY_ERROR; }

  // Further parsing utilities.
  // Given a control frame, parse out a SpdyHeaderBlock.  Only
  // valid for SYN_STREAM and SYN_REPLY frames.
  // Returns true if successfully parsed, false otherwise.
  bool ParseHeaderBlock(const SpdyFrame* frame, SpdyHeaderBlock* block);

  // Create a SpdySynStreamControlFrame.
  // |stream_id| is the id for this stream.
  // |associated_stream_id| is the associated stream id for this stream.
  // |priority| is the priority (0-3) for this stream.
  // |flags| is the flags to use with the data.
  //    To mark this frame as the last frame, enable CONTROL_FLAG_FIN.
  // |compressed| specifies whether the frame should be compressed.
  // |headers| is the header block to include in the frame.
  SpdySynStreamControlFrame* CreateSynStream(SpdyStreamId stream_id,
                                             SpdyStreamId associated_stream_id,
                                             int priority,
                                             SpdyControlFlags flags,
                                             bool compressed,
                                             SpdyHeaderBlock* headers);

  static SpdyRstStreamControlFrame* CreateRstStream(SpdyStreamId stream_id,
                                                    int status);

  // Create a SpdySynReplyControlFrame.
  // |stream_id| is the stream for this frame.
  // |flags| is the flags to use with the data.
  //    To mark this frame as the last frame, enable CONTROL_FLAG_FIN.
  // |compressed| specifies whether the frame should be compressed.
  // |headers| is the header block to include in the frame.
  SpdySynReplyControlFrame* CreateSynReply(SpdyStreamId stream_id,
                                           SpdyControlFlags flags,
                                           bool compressed,
                                           SpdyHeaderBlock* headers);

  // Create a data frame.
  // |stream_id| is the stream  for this frame
  // |data| is the data to be included in the frame.
  // |len| is the length of the data
  // |flags| is the flags to use with the data.
  //    To create a compressed frame, enable DATA_FLAG_COMPRESSED.
  //    To mark this frame as the last data frame, enable DATA_FLAG_FIN.
  SpdyDataFrame* CreateDataFrame(SpdyStreamId stream_id, const char* data,
                                 uint32 len, SpdyDataFlags flags);

  static SpdyControlFrame* CreateNopFrame();

  // NOTES about frame compression.
  // We want spdy to compress headers across the entire session.  As long as
  // the session is over TCP, frames are sent serially.  The client & server
  // can each compress frames in the same order and then compress them in that
  // order, and the remote can do the reverse.  However, we ultimately want
  // the creation of frames to be less sensitive to order so that they can be
  // placed over a UDP based protocol and yet still benefit from some
  // compression.  We don't know of any good compression protocol which does
  // not build its state in a serial (stream based) manner....  For now, we're
  // using zlib anyway.

  // Compresses a SpdyFrame.
  // On success, returns a new SpdyFrame with the payload compressed.
  // Compression state is maintained as part of the SpdyFramer.
  // Returned frame must be freed with "delete".
  // On failure, returns NULL.
  SpdyFrame* CompressFrame(const SpdyFrame* frame);

  // Decompresses a SpdyFrame.
  // On success, returns a new SpdyFrame with the payload decompressed.
  // Compression state is maintained as part of the SpdyFramer.
  // Returned frame must be freed with "delete".
  // On failure, returns NULL.
  SpdyFrame* DecompressFrame(const SpdyFrame* frame);

  // Create a copy of a frame.
  // Returned frame must be freed with "delete".
  SpdyFrame* DuplicateFrame(const SpdyFrame* frame);

  // For debugging.
  static const char* StateToString(int state);
  static const char* ErrorCodeToString(int error_code);

  // Export the compression dictionary
  static const char kDictionary[];
  static const int kDictionarySize;

 protected:
  FRIEND_TEST(SpdyFramerTest, HeaderBlockBarfsOnOutOfOrderHeaders);
  friend class net::SpdyNetworkTransactionTest;
  friend class net::HttpNetworkLayer;  // This is temporary for the server.
  friend class test::TestSpdyVisitor;
  friend void test::FramerSetEnableCompressionHelper(SpdyFramer* framer,
                                                     bool compress);

  // For ease of testing we can tweak compression on/off.
  void set_enable_compression(bool value);
  static void set_enable_compression_default(bool value);

 private:
  // Internal breakout from ProcessInput.  Returns the number of bytes
  // consumed from the data.
  size_t ProcessCommonHeader(const char* data, size_t len);
  void ProcessControlFrameHeader();
  size_t ProcessControlFramePayload(const char* data, size_t len);
  size_t ProcessDataFramePayload(const char* data, size_t len);

  // Initialize the ZLib state.
  bool InitializeCompressor();
  bool InitializeDecompressor();

  // Not used (yet)
  size_t BytesSafeToRead() const;

  // Set the error code and moves the framer into the error state.
  void set_error(SpdyError error);

  // Expands the control frame buffer to accomodate a particular payload size.
  void ExpandControlFrameBuffer(size_t size);

  // Given a frame, breakdown the variable payload length, the static header
  // header length, and variable payload pointer.
  bool GetFrameBoundaries(const SpdyFrame* frame, int* payload_length,
                          int* header_length, const char** payload) const;

  SpdyState state_;
  SpdyError error_code_;
  size_t remaining_payload_;
  size_t remaining_control_payload_;

  char* current_frame_buffer_;
  size_t current_frame_len_;  // Number of bytes read into the current_frame_.
  size_t current_frame_capacity_;

  bool enable_compression_;
  scoped_ptr<z_stream> compressor_;
  scoped_ptr<z_stream> decompressor_;
  SpdyFramerVisitorInterface* visitor_;

  static bool compression_default_;
};

}  // namespace spdy

#endif  // NET_SPDY_SPDY_FRAMER_H_