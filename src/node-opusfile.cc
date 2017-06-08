
#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <node_object_wrap.h>
#include "common.h"
#include <nan.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include "../opusfile/include/opusfile.h"
#include <opus/opus.h>
#include <ogg/ogg.h>

using namespace node;
using namespace v8;

#ifndef max
#define max(x, y) ((x) > (y)) ? (x) : (y)
#endif
#ifndef min
#define min(x, y) ((x) < (y)) ? (x) : (y)
#endif

typedef struct {
    int version;
    int channels; /* Number of channels: 1..255 */
    int preskip;
    ogg_uint32_t input_sample_rate;
    int gain; /* in dB S7.8 should be zero whenever possible */
    int channel_mapping;
    /* The rest is only used if channel_mapping != 0 */
    int nb_streams;
    int nb_coupled;
    unsigned char stream_map[255];
} OpusHeader;

typedef struct {
    void *readdata;
    opus_int64 total_samples_per_channel;
    int rawmode;
    int channels;
    long rate;
    int gain;
    int samplesize;
    int endianness;
    char *infilename;
    int ignorelength;
    int skip;
    int extraout;
    char *comments;
    int comments_length;
    int copy_comments;
} oe_enc_opt;

typedef struct {
    unsigned char *data;
    int maxlen;
    int pos;
} Packet;

const opus_int32 bitrate = 16000;
const opus_int32 rate = 16000;
const opus_int32 frame_size = 960;
const int with_cvbr = 1;
const int max_ogg_delay = 0;
const int comment_padding = 512;

opus_int32 coding_rate = 16000;
ogg_int32_t _packetId;
OpusEncoder *_encoder = 0;
uint8_t *_packet = 0;
ogg_stream_state os;
FILE *_fileOs = 0;
oe_enc_opt inopt;
OpusHeader header;
opus_int32 min_bytes;
int max_frame_bytes;
ogg_packet op;
ogg_page og;
opus_int64 bytes_written;
opus_int64 pages_out;
opus_int64 total_samples;
ogg_int64_t enc_granulepos;
ogg_int64_t last_granulepos;
int size_segments;
int last_segments;

static int write_uint32(Packet *p, ogg_uint32_t val) {
    if (p->pos > p->maxlen - 4) {
        return 0;
    }
    p->data[p->pos  ] = (val    ) & 0xFF;
    p->data[p->pos+1] = (val>> 8) & 0xFF;
    p->data[p->pos+2] = (val>>16) & 0xFF;
    p->data[p->pos+3] = (val>>24) & 0xFF;
    p->pos += 4;
    return 1;
}

static int write_uint16(Packet *p, ogg_uint16_t val) {
    if (p->pos > p->maxlen-2) {
        return 0;
    }
    p->data[p->pos  ] = (val    ) & 0xFF;
    p->data[p->pos+1] = (val>> 8) & 0xFF;
    p->pos += 2;
    return 1;
}

static int write_chars(Packet *p, const unsigned char *str, int nb_chars)
{
    int i;
    if (p->pos>p->maxlen-nb_chars)
        return 0;
    for (i=0;i<nb_chars;i++)
        p->data[p->pos++] = str[i];
    return 1;
}

void cleanupRecorder() {
  if (_encoder) {
    opus_encoder_destroy(_encoder);
    _encoder = 0;
  }

  ogg_stream_clear(&os);

  if (_packet) {
    free(_packet);
    _packet = 0;
  }

  if (_fileOs) {
    fclose(_fileOs);
    _fileOs = 0;
  }

  _packetId = -1;
  bytes_written = 0;
  pages_out = 0;
  total_samples = 0;
  enc_granulepos = 0;
  size_segments = 0;
  last_segments = 0;
  last_granulepos = 0;
  memset(&os, 0, sizeof(ogg_stream_state));
  memset(&inopt, 0, sizeof(oe_enc_opt));
  memset(&header, 0, sizeof(OpusHeader));
  memset(&op, 0, sizeof(ogg_packet));
  memset(&og, 0, sizeof(ogg_page));

  // fprintf(stderr, "Recording ends!!!\n");
}

int opus_header_to_packet_(const OpusHeader *h, unsigned char *packet, int len) {
    int i;
    Packet p;
    unsigned char ch;

    p.data = packet;
    p.maxlen = len;
    p.pos = 0;
    if (len < 19) {
        return 0;
    }
    if (!write_chars(&p, (const unsigned char *)"OpusHead", 8)) {
        return 0;
    }

    ch = 1;
    if (!write_chars(&p, &ch, 1)) {
        return 0;
    }

    ch = h->channels;
    if (!write_chars(&p, &ch, 1)) {
        return 0;
    }

    if (!write_uint16(&p, h->preskip)) {
        return 0;
    }

    if (!write_uint32(&p, h->input_sample_rate)) {
        return 0;
    }

    if (!write_uint16(&p, h->gain)) {
        return 0;
    }

    ch = h->channel_mapping;
    if (!write_chars(&p, &ch, 1)) {
        return 0;
    }

    if (h->channel_mapping != 0) {
        ch = h->nb_streams;
        if (!write_chars(&p, &ch, 1)) {
            return 0;
        }

        ch = h->nb_coupled;
        if (!write_chars(&p, &ch, 1)) {
            return 0;
        }

        /* Multi-stream support */
        for (i = 0; i < h->channels; i++) {
            if (!write_chars(&p, &h->stream_map[i], 1)) {
                return 0;
            }
        }
    }

    return p.pos;
}

#define writeint(buf, base, val) do { buf[base + 3] = ((val) >> 24) & 0xff; \
buf[base + 2]=((val) >> 16) & 0xff; \
buf[base + 1]=((val) >> 8) & 0xff; \
buf[base] = (val) & 0xff; \
} while(0)

static void comment_init(char **comments, int *length, const char *vendor_string) {
    /* The 'vendor' field should be the actual encoding library used */
    int vendor_length = strlen(vendor_string);
    int user_comment_list_length = 0;
    int len = 8 + 4 + vendor_length + 4;
    char *p = (char *)malloc(len);
    memcpy(p, "OpusTags", 8);
    writeint(p, 8, vendor_length);
    memcpy(p + 12, vendor_string, vendor_length);
    writeint(p, 12 + vendor_length, user_comment_list_length);
    *length = len;
    *comments = p;
}

static void comment_pad(char **comments, int* length, int amount) {
    if (amount > 0) {
        char *p = *comments;
        /* Make sure there is at least amount worth of padding free, and round up to the maximum that fits in the current ogg segments */
        int newlen = (*length + amount + 255) / 255 * 255 - 1;
        p = static_cast<char*>(realloc(p, newlen));
        int i = 0;
        for (i = *length; i < newlen; i++) {
            p[i] = 0;
        }
        *comments = p;
        *length = newlen;
    }
}

static int writeOggPage(ogg_page *page, FILE *os) {
    int written = fwrite(page->header, sizeof(unsigned char), page->header_len, os);
    written += fwrite(page->body, sizeof(unsigned char), page->body_len, os);
    return written;
}

int initRecorder(const char *path) {
  cleanupRecorder();

  // fprintf(stderr, "in Recorder, path: %s\n", path);
  if (!path) {
    return 0;
  }

  _fileOs = fopen(path, "wb");
  if (!_fileOs) {
    return 0;
  }

  inopt.rate = rate;
  inopt.gain = 0;
  inopt.endianness = 0;
  inopt.copy_comments = 0;
  inopt.rawmode = 1;
  inopt.ignorelength = 1;
  inopt.samplesize = 16;
  inopt.channels = 1;
  inopt.skip = 0;

  comment_init(&inopt.comments, &inopt.comments_length, opus_get_version_string());

  if (rate > 24000) {
    coding_rate = 48000;
  } else if (rate > 16000) {
    coding_rate = 24000;
  } else if (rate > 12000) {
    coding_rate = 16000;
  } else if (rate > 8000) {
    coding_rate = 12000;
  } else {
    coding_rate = 8000;
  }

  /*   frame_size=frame_size/(48000/coding_rate); */
  if (rate != coding_rate) {
    fprintf(stderr, "Invalid rate\n");
    return 0;
  }

  header.channels = 1;
  header.channel_mapping = 0;
  header.input_sample_rate = rate;
  header.gain = inopt.gain;
  header.nb_streams = 1;

  int result = OPUS_OK;
  _encoder = opus_encoder_create(coding_rate, 1, OPUS_APPLICATION_AUDIO, &result);
  if (result != OPUS_OK) {
    fprintf(stderr, "Error cannot create encoder: %s\n", opus_strerror(result));
    return 0;
  }

  min_bytes = max_frame_bytes = (1275 * 3 + 7) * header.nb_streams;
  _packet = static_cast<unsigned char*>(malloc(max_frame_bytes));

  result = opus_encoder_ctl(_encoder, OPUS_SET_BITRATE(bitrate));
  if (result != OPUS_OK) {
    fprintf(stderr, "Error OPUS_SET_BITRATE returned: %s\n", opus_strerror(result));
    return 0;
  }

#ifdef OPUS_SET_LSB_DEPTH
  result = opus_encoder_ctl(_encoder, OPUS_SET_LSB_DEPTH(max(8, min(24, inopt.samplesize))));
  if (result != OPUS_OK) {
    fprintf(stderr, "Warning OPUS_SET_LSB_DEPTH returned: %s\n", opus_strerror(result));
  }
#endif

  opus_int32 lookahead;
  result = opus_encoder_ctl(_encoder, OPUS_GET_LOOKAHEAD(&lookahead));
  if (result != OPUS_OK) {
    fprintf(stderr, "Error OPUS_GET_LOOKAHEAD returned: %s\n", opus_strerror(result));
    return 0;
  }

  inopt.skip += lookahead;
  header.preskip = (int)(inopt.skip * (48000.0 / coding_rate));
  inopt.extraout = (int)(header.preskip * (rate / 48000.0));

  if (ogg_stream_init(&os, rand()) == -1) {
    fprintf(stderr, "Error: stream init failed");
    return 0;
  }

  unsigned char header_data[100];
  int packet_size = opus_header_to_packet_(&header, header_data, 100);
  op.packet = header_data;
  op.bytes = packet_size;
  op.b_o_s = 1;
  op.e_o_s = 0;
  op.granulepos = 0;
  op.packetno = 0;
  ogg_stream_packetin(&os, &op);

  while ((result = ogg_stream_flush(&os, &og))) {
    if (!result) {
      break;
    }

    int pageBytesWritten = writeOggPage(&og, _fileOs);
    if (pageBytesWritten != og.header_len + og.body_len) {
      fprintf(stderr, "Error: failed writing header to output stream");
      return 0;
    }
    bytes_written += pageBytesWritten;
    pages_out++;
  }

  comment_pad(&inopt.comments, &inopt.comments_length, comment_padding);
  op.packet = (unsigned char *)inopt.comments;
  op.bytes = inopt.comments_length;
  op.b_o_s = 0;
  op.e_o_s = 0;
  op.granulepos = 0;
  op.packetno = 1;
  ogg_stream_packetin(&os, &op);

  while ((result = ogg_stream_flush(&os, &og))) {
    if (result == 0) {
      break;
    }

    int writtenPageBytes = writeOggPage(&og, _fileOs);
    if (writtenPageBytes != og.header_len + og.body_len) {
      fprintf(stderr, "Error: failed writing header to output stream");
      return 0;
    }

    bytes_written += writtenPageBytes;
    pages_out++;
  }

  free(inopt.comments);

  return 1;
}

int writeFrame(uint8_t *framePcmBytes, unsigned int frameByteCount) {
    int cur_frame_size = frame_size;
    _packetId++;

    opus_int32 nb_samples = frameByteCount / 2;
    total_samples += nb_samples;
    if (nb_samples < frame_size) {
        op.e_o_s = 1;
    } else {
        op.e_o_s = 0;
    }

    int nbBytes = 0;

    if (nb_samples != 0) {
        uint8_t *paddedFrameBytes = framePcmBytes;
        int freePaddedFrameBytes = 0;

        if (nb_samples < cur_frame_size) {
            paddedFrameBytes = static_cast<unsigned char*>(malloc(cur_frame_size * 2));
            freePaddedFrameBytes = 1;
            memcpy(paddedFrameBytes, framePcmBytes, frameByteCount);
            memset(paddedFrameBytes + nb_samples * 2, 0, cur_frame_size * 2 - nb_samples * 2);
        }

        nbBytes = opus_encode(_encoder, (opus_int16 *)paddedFrameBytes, cur_frame_size, _packet, max_frame_bytes / 10);
        if (freePaddedFrameBytes) {
            free(paddedFrameBytes);
            paddedFrameBytes = NULL;
        }

        if (nbBytes < 0) {
            fprintf(stderr, "Encoding failed: %s. Aborting.\n", opus_strerror(nbBytes));
            return 0;
        }

        enc_granulepos += cur_frame_size * 48000 / coding_rate;
        size_segments = (nbBytes + 255) / 255;
        min_bytes = min(nbBytes, min_bytes);
    }

    while ((((size_segments <= 255) && (last_segments + size_segments > 255)) || (enc_granulepos - last_granulepos > max_ogg_delay)) && ogg_stream_flush_fill(&os, &og, 255 * 255)) {
        if (ogg_page_packets(&og) != 0) {
            last_granulepos = ogg_page_granulepos(&og);
        }

        last_segments -= og.header[26];
        int writtenPageBytes = writeOggPage(&og, _fileOs);
        if (writtenPageBytes != og.header_len + og.body_len) {
            fprintf(stderr, "Error: failed writing data to output stream\n");
            return 0;
        }
        bytes_written += writtenPageBytes;

        pages_out++;
    }

    op.packet = (unsigned char *)_packet;
    op.bytes = nbBytes;
    op.b_o_s = 0;
    op.granulepos = enc_granulepos;
    if (op.e_o_s) {
        op.granulepos = ((total_samples * 48000 + rate - 1) / rate) + header.preskip;
    }
    op.packetno = 2 + _packetId;
    ogg_stream_packetin(&os, &op);
    last_segments += size_segments;

    while ((op.e_o_s || (enc_granulepos + (frame_size * 48000 / coding_rate) - last_granulepos > max_ogg_delay) || (last_segments >= 255)) ? ogg_stream_flush_fill(&os, &og, 255 * 255) : ogg_stream_pageout_fill(&os, &og, 255 * 255)) {
        if (ogg_page_packets(&og) != 0) {
            last_granulepos = ogg_page_granulepos(&og);
        }
        last_segments -= og.header[26];
        int writtenPageBytes = writeOggPage(&og, _fileOs);
        if (writtenPageBytes != og.header_len + og.body_len) {
            fprintf(stderr, "Error: failed writing data to output stream\n");
            return 0;
        }
        bytes_written += writtenPageBytes;
        pages_out++;
    }

    // fprintf(stderr, "last byte_written is %lld\n", bytes_written);
    return 1;
}

#define FRAME_SIZE 960
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define ENCODER_SIZE 133
#define MAX_BUFFER_SIZE 1920

void Normalize(const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  int _argc = args.Length();

  if(_argc != 2) {
    isolate->ThrowException(
      Exception::TypeError(
        v8::String::NewFromUtf8(isolate, "Usage: ./opusaudio_example <input.opus> <output.opus>")
      )
    );
    return;
  }

  v8::String::Utf8Value argv0(args[0]->ToString());
  v8::String::Utf8Value argv1(args[1]->ToString());
  std::string _argv0 = std::string(*argv0);
  std::string _argv1 = std::string(*argv1);

  char *inFile;
  FILE *fin;
  unsigned char bytes[ENCODER_SIZE];
  unsigned char pcm_frame_2[MAX_BUFFER_SIZE];
  int result;
  int error;
  OpusDecoder *decoder;
  int res = 0;
  int i = 0;

#if defined(_WIN32)
  // win32_utf8_setup(&_argc,&_argv);
#endif

  inFile = new char[strlen(_argv0.c_str())];
  strcpy(inFile, _argv0.c_str());

  fin = fopen(inFile, "rb");
  if (fin == NULL) {
     fprintf(stderr, "\nfailed to open input file: %s", strerror(errno));
     // return EXIT_FAILURE;
  }

  result = initRecorder(_argv1.c_str());
  if (result != 1) {
    fprintf(stderr, "\nresult: %s", opus_strerror(result));
    // return EXIT_FAILURE;
  }

  decoder = opus_decoder_create(SAMPLE_RATE, CHANNELS, &error);
  if (error != 0) {
    fprintf(stderr, "\nerror: %s", opus_strerror(error));
    // return EXIT_FAILURE;
  }

  while (!feof(fin)) {
    i++;
    fread(bytes, sizeof(unsigned char), ENCODER_SIZE, fin);
    res = opus_decode(decoder, bytes, ENCODER_SIZE, (short *)(pcm_frame_2), FRAME_SIZE, 0);
    if (res < 0) {
      fprintf(stderr, "\nres: %d decoder: %s", res, opus_strerror(res));
      // return EXIT_FAILURE;
    }
    writeFrame(pcm_frame_2, MAX_BUFFER_SIZE);
  }

  opus_decoder_destroy(decoder);
  cleanupRecorder();
  fclose(fin);
  
  auto message = Nan::New<v8::String>("I'm a Node Hero!").ToLocalChecked();
  args.GetReturnValue().Set(message);
}

void Initialize(v8::Local<v8::Object> exports) {
  NODE_SET_METHOD(exports, "Normalize", Normalize);
}

NODE_MODULE(module_name, Initialize)
