
#include <v8.h>
#include <node.h>
#include <node_buffer.h>
#include <node_object_wrap.h>
#include <opus/opus.h>
#include "../opusfile/include/opusfile.h"
#include "common.h"
#include <nan.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

using namespace node;
using namespace v8;

#define FRAME_SIZE 960
#define MAX_FRAME_SIZE 6*960
#define MAX_PACKET_SIZE (3*1276)
#define BITRATE 64000

const char* getDecodeError( int decodedSamples ) {
  switch( decodedSamples ) {
    case OPUS_BAD_ARG:
      return "One or more invalid/out of range arguments";
    case OPUS_BUFFER_TOO_SMALL:
      return "The mode struct passed is invalid";
    case OPUS_INTERNAL_ERROR:
      return "An internal error was detected";
    case OPUS_INVALID_PACKET:
      return "The compressed data passed is corrupted";
    case OPUS_UNIMPLEMENTED:
      return "Invalid/unsupported request number.";
    case OPUS_INVALID_STATE:
      return "An encoder or decoder structure is invalid or already freed.";
    case OPUS_ALLOC_FAIL:
      return "Memory allocation has failed";
    default:
      return "Unknown OPUS error";
  }
}

class OpusEncoder : public ObjectWrap {
  private:
    OpusEncoder* encoder;
    OpusDecoder* decoder;

    opus_int32 rate;
    int channels;
    int application;

    unsigned char outOpus[ MAX_PACKET_SIZE ];
    opus_int16* outPcm;

static void print_duration(FILE *_fp,ogg_int64_t _nsamples,int _frac){
  ogg_int64_t seconds;
  ogg_int64_t minutes;
  ogg_int64_t hours;
  ogg_int64_t days;
  ogg_int64_t weeks;
  _nsamples+=_frac?24:24000;
  seconds=_nsamples/48000;
  _nsamples-=seconds*48000;
  minutes=seconds/60;
  seconds-=minutes*60;
  hours=minutes/60;
  minutes-=hours*60;
  days=hours/24;
  hours-=days*24;
  weeks=days/7;
  days-=weeks*7;
  if(weeks)fprintf(_fp,"%liw",(long)weeks);
  if(weeks||days)fprintf(_fp,"%id",(int)days);
  if(weeks||days||hours){
    if(weeks||days)fprintf(_fp,"%02ih",(int)hours);
    else fprintf(_fp,"%ih",(int)hours);
  }
  if(weeks||days||hours||minutes){
    if(weeks||days||hours)fprintf(_fp,"%02im",(int)minutes);
    else fprintf(_fp,"%im",(int)minutes);
    fprintf(_fp,"%02i",(int)seconds);
  }
  else fprintf(_fp,"%i",(int)seconds);
  if(_frac)fprintf(_fp,".%03i",(int)(_nsamples/48));
  fprintf(_fp,"s");
}

static void print_size(FILE *_fp,opus_int64 _nbytes,int _metric,
 const char *_spacer){
  static const char SUFFIXES[7]={' ','k','M','G','T','P','E'};
  opus_int64 val;
  opus_int64 den;
  opus_int64 round;
  int        base;
  int        shift;
  base=_metric?1000:1024;
  round=0;
  den=1;
  for(shift=0;shift<6;shift++){
    if(_nbytes<den*base-round)break;
    den*=base;
    round=den>>1;
  }
  val=(_nbytes+round)/den;
  if(den>1&&val<10){
    if(den>=1000000000)val=(_nbytes+(round/100))/(den/100);
    else val=(_nbytes*100+round)/den;
    fprintf(_fp,"%li.%02i%s%c",(long)(val/100),(int)(val%100),
     _spacer,SUFFIXES[shift]);
  }
  else if(den>1&&val<100){
    if(den>=1000000000)val=(_nbytes+(round/10))/(den/10);
    else val=(_nbytes*10+round)/den;
    fprintf(_fp,"%li.%i%s%c",(long)(val/10),(int)(val%10),
     _spacer,SUFFIXES[shift]);
  }
  else fprintf(_fp,"%li%s%c",(long)val,_spacer,SUFFIXES[shift]);
}

static void put_le32(unsigned char *_dst,opus_uint32 _x){
  _dst[0]=(unsigned char)(_x&0xFF);
  _dst[1]=(unsigned char)(_x>>8&0xFF);
  _dst[2]=(unsigned char)(_x>>16&0xFF);
  _dst[3]=(unsigned char)(_x>>24&0xFF);
}

/*Make a header for a 48 kHz, stereo, signed, 16-bit little-endian PCM WAV.*/
static void make_wav_header(unsigned char _dst[44],ogg_int64_t _duration){
  /*The chunk sizes are set to 0x7FFFFFFF by default.
    Many, though not all, programs will interpret this to mean the duration is
     "undefined", and continue to read from the file so long as there is actual
     data.*/
  static const unsigned char WAV_HEADER_TEMPLATE[44]={
    'R','I','F','F',0xFF,0xFF,0xFF,0x7F,
    'W','A','V','E','f','m','t',' ',
    0x10,0x00,0x00,0x00,0x01,0x00,0x02,0x00,
    0x80,0xBB,0x00,0x00,0x00,0xEE,0x02,0x00,
    0x04,0x00,0x10,0x00,'d','a','t','a',
    0xFF,0xFF,0xFF,0x7F
  };
  memcpy(_dst,WAV_HEADER_TEMPLATE,sizeof(WAV_HEADER_TEMPLATE));
  if(_duration>0){
    if(_duration>0x1FFFFFF6){
      fprintf(stderr,"WARNING: WAV output would be larger than 2 GB.\n");
      fprintf(stderr,
       "Writing non-standard WAV header with invalid chunk sizes.\n");
    }
    else{
      opus_uint32 audio_size;
      audio_size=(opus_uint32)(_duration*4);
      put_le32(_dst+4,audio_size+36);
      put_le32(_dst+40,audio_size);
    }
  }
}
  protected:
    int EnsureEncoder() {
      if( encoder != NULL ) return 0;
      int error;
      encoder = opus_encoder_create( rate, channels, application, &error );
      return error;
    }
    int EnsureDecoder() {
      if( decoder != NULL ) return 0;
      int error;
      decoder = opus_decoder_create( rate, channels, &error );
      return error;
    }

  public:
      OpusEncoder( opus_int32 rate, int channels, int application ):
      encoder( NULL ), decoder( NULL ),
      rate( rate ), channels( channels ), application( application ) {

      outPcm = new opus_int16[ channels * MAX_FRAME_SIZE ];
    }

    ~OpusEncoder() {
      if( encoder != NULL )
        opus_encoder_destroy( encoder );
      if( decoder != NULL )
        opus_decoder_destroy( decoder );

      encoder = NULL;
      decoder = NULL;

      delete outPcm;
      outPcm = NULL;
    }

    static void Encode( const Nan::FunctionCallbackInfo< v8::Value >& info ) {

      // Unwrap the encoder.
      OpusEncoder* self = ObjectWrap::Unwrap<OpusEncoder>( info.This() );
      if( self->EnsureEncoder() != OPUS_OK ) {
        Nan::ThrowError( "Could not create encoder. Check the encoder parameters" );
        return;
      }


      // Read the function arguments
      REQ_OBJ_ARG( 0, pcmBuffer );
      OPT_INT_ARG( 1, maxPacketSize, MAX_PACKET_SIZE );

      // Read the PCM data.
      char* pcmData = Buffer::Data(pcmBuffer);
      opus_int16* pcm = reinterpret_cast<opus_int16*>( pcmData );
      int frameSize = Buffer::Length( pcmBuffer ) / 2 / self->channels;

      // Encode the samples.
      int compressedLength = opus_encode( self->encoder, pcm, frameSize, &(self->outOpus[0]), maxPacketSize );

      // Create a new result buffer.
      Nan::MaybeLocal<Object> actualBuffer = Nan::CopyBuffer(reinterpret_cast<char*>(self->outOpus), compressedLength );
      if( !actualBuffer.IsEmpty() )
        info.GetReturnValue().Set( actualBuffer.ToLocalChecked() );
    }

    static void Decode( const Nan::FunctionCallbackInfo< v8::Value >& info ) {

      REQ_OBJ_ARG( 0, compressedBuffer );

      // Read the compressed data.
      unsigned char* compressedData = (unsigned char*)Buffer::Data( compressedBuffer );
      size_t compressedDataLength = Buffer::Length( compressedBuffer );

      OpusEncoder* self = ObjectWrap::Unwrap<OpusEncoder>( info.This() );
      if( self->EnsureDecoder() != OPUS_OK ) {
        Nan::ThrowError( "Could not create decoder. Check the decoder parameters" );
        return;
      }

      // Encode the samples.
      int decodedSamples = opus_decode(
          self->decoder,
          compressedData,
          compressedDataLength,
          &(self->outPcm[0]),
            MAX_FRAME_SIZE, /* decode_fex */ 0 );

      if( decodedSamples < 0 ) {
        return Nan::ThrowTypeError( getDecodeError( decodedSamples ) );
      }

      // Create a new result buffer.
      int decodedLength = decodedSamples * 2 * self->channels;
      Nan::MaybeLocal<Object> actualBuffer = Nan::CopyBuffer( reinterpret_cast<char*>(self->outPcm), decodedLength );
      if( !actualBuffer.IsEmpty() )
        info.GetReturnValue().Set( actualBuffer.ToLocalChecked() );
    }

    static void ApplyEncoderCTL( const Nan::FunctionCallbackInfo< v8::Value >&info ) {

      REQ_INT_ARG( 0, ctl );
      REQ_INT_ARG( 1, value );
      
      OpusEncoder* self = ObjectWrap::Unwrap<OpusEncoder>( info.This() );
      if ( self->EnsureEncoder() != OPUS_OK ) {
        Nan::ThrowError( "Could not create encoder. Check the encoder parameters" );
        return;
      }

      if( opus_encoder_ctl( self->encoder, ctl, value ) != OPUS_OK )
        return Nan::ThrowError( "Invalid ctl/value" );
    }

    static void ApplyDecoderCTL( const Nan::FunctionCallbackInfo< v8::Value >&info ) {

      REQ_INT_ARG( 0, ctl );
      REQ_INT_ARG( 1, value );

      OpusEncoder* self = ObjectWrap::Unwrap<OpusEncoder>( info.This() );
      if ( self->EnsureDecoder() != OPUS_OK ) {
        Nan::ThrowError( "Could not create decoder. Check the decoder parameters" );
        return;
      }

      if ( opus_decoder_ctl( self->decoder, ctl, value ) != OPUS_OK)
        return Nan::ThrowError( "Invalid ctl/value" );
    }

    static void SetBitrate( const Nan::FunctionCallbackInfo< v8::Value >& info ) {

      REQ_INT_ARG( 0, bitrate );

      OpusEncoder* self = ObjectWrap::Unwrap<OpusEncoder>( info.This() );
      if( self->EnsureEncoder() != OPUS_OK ) {
        Nan::ThrowError( "Could not create encoder. Check the encoder parameters" );
        return;
      }

      if( opus_encoder_ctl( self->encoder, OPUS_SET_BITRATE( bitrate ) ) != OPUS_OK )
        return Nan::ThrowError( "Invalid bitrate" );
    }

    static void GetBitrate( const Nan::FunctionCallbackInfo< v8::Value >& info ) {

      OpusEncoder* self = ObjectWrap::Unwrap<OpusEncoder>( info.This() );
      if( self->EnsureEncoder() != OPUS_OK ) {
        Nan::ThrowError( "Could not create encoder. Check the encoder parameters" );
        return;
      }

      opus_int32 bitrate;
      opus_encoder_ctl( self->encoder, OPUS_GET_BITRATE( &bitrate ) );

      info.GetReturnValue().Set( Nan::New<v8::Integer>( bitrate ) );
    }

    static void New( const Nan::FunctionCallbackInfo< v8::Value >& info ) {

      if( !info.IsConstructCall() ) {
        return Nan::ThrowTypeError("Use the new operator to construct the OpusEncoder.");
      }

      OPT_INT_ARG( 0, rate, 48000 );
      OPT_INT_ARG( 1, channels, 1 );
      OPT_INT_ARG( 2, application, OPUS_APPLICATION_AUDIO );

      OpusEncoder* encoder = new OpusEncoder( rate, channels, application );

      encoder->Wrap( info.This() );
      info.GetReturnValue().Set( info.This() );
    }

    static void WhoAmI(const Nan::FunctionCallbackInfo<v8::Value>& args) {  
      v8::Isolate* isolate = args.GetIsolate();
      int _argc = args.Length();

      v8::String::Utf8Value argv0(args[0]->ToString());
      v8::String::Utf8Value argv1(args[1]->ToString());
      std::string _argv0 = std::string(*argv0);
      std::string _argv1 = std::string(*argv1);

      if(_argc != 2) {
        isolate->ThrowException(
          Exception::TypeError(
            v8::String::NewFromUtf8(isolate, _argv0.c_str())
          )
        );
        return;
      }

  OggOpusFile  *of = NULL;
  ogg_int64_t   duration;
  unsigned char wav_header[44];
  int           ret;
  int           is_ssl;
  int           output_seekable;
#if defined(_WIN32)
  // win32_utf8_setup(&_argc,&_argv);
#endif
  if(_argc!=2){
    fprintf(stderr,"Usage: %s <file.opus>\n",_argv0.c_str());
    //return EXIT_FAILURE;
  }

  is_ssl=0;
  if(strcmp(_argv1.c_str(),"-")==0){
    OpusFileCallbacks cb={NULL,NULL,NULL,NULL};
    of=op_open_callbacks(op_fdopen(&cb,fileno(stdin),"rb"),&cb,NULL,0,&ret);
  }
  else{
    //OpusServerInfo info;
    /*Try to treat the argument as a URL.*/
    //of=op_open_url(_argv[1],&ret,OP_GET_SERVER_INFO(&info),NULL);
#if 0
    /*
    if(of==NULL){
      OpusFileCallbacks  cb={NULL,NULL,NULL,NULL};
      void              *fp;
      /*For debugging: force a file to not be seekable.*/
      fp=op_fopen(&cb,_argv1,"rb");
      cb.seek=NULL;
      cb.tell=NULL;
      of=op_open_callbacks(fp,&cb,NULL,0,NULL);
    }
    */
#else
    //if(of==NULL)of=op_open_file(_argv0.c_str(),&ret);
#endif
    /*
    else{
      if(info.name!=NULL){
        fprintf(stderr,"Station name: %s\n",info.name);
      }
      if(info.description!=NULL){
        fprintf(stderr,"Station description: %s\n",info.description);
      }
      if(info.genre!=NULL){
        fprintf(stderr,"Station genre: %s\n",info.genre);
      }
      if(info.url!=NULL){
        fprintf(stderr,"Station homepage: %s\n",info.url);
      }
      if(info.bitrate_kbps>=0){
        fprintf(stderr,"Station bitrate: %u kbps\n",
         (unsigned)info.bitrate_kbps);
      }
      if(info.is_public>=0){
        fprintf(stderr,"%s\n",
         info.is_public?"Station is public.":"Station is private.");
      }
      if(info.server!=NULL){
        fprintf(stderr,"Server software: %s\n",info.server);
      }
      if(info.content_type!=NULL){
        fprintf(stderr,"Content-Type: %s\n",info.content_type);
      }
      is_ssl=info.is_ssl;
      opus_server_info_clear(&info);
    }
    */
  }
  if(of==NULL)of=op_open_file(_argv1.c_str(),&ret);
  if(of==NULL){
    fprintf(stderr,"Failed to open file '%s': %i\n",_argv1.c_str(),ret);
    //return EXIT_FAILURE;
  }
  duration=0;
  output_seekable=fseek(stdout,0,SEEK_CUR)!=-1;
  if(op_seekable(of)){
    opus_int64  size;
    fprintf(stderr,"Total number of links: %i\n",op_link_count(of));
    duration=op_pcm_total(of,-1);
    fprintf(stderr,"Total duration: ");
    print_duration(stderr,duration,3);
    fprintf(stderr," (%li samples @ 48 kHz)\n",(long)duration);
    size=op_raw_total(of,-1);
    fprintf(stderr,"Total size: ");
    print_size(stderr,size,0,"");
    fprintf(stderr,"\n");
  }
  else if(!output_seekable){
    fprintf(stderr,"WARNING: Neither input nor output are seekable.\n");
    fprintf(stderr,
     "Writing non-standard WAV header with invalid chunk sizes.\n");
  }
  make_wav_header(wav_header,duration);
  if(!fwrite(wav_header,sizeof(wav_header),1,stdout)){
    fprintf(stderr,"Error writing WAV header: %s\n",strerror(errno));
    ret=EXIT_FAILURE;
  }
  else{
    ogg_int64_t pcm_offset;
    ogg_int64_t pcm_print_offset;
    ogg_int64_t nsamples;
    opus_int32  bitrate;
    int         prev_li;
    prev_li=-1;
    nsamples=0;
    pcm_offset=op_pcm_tell(of);
    if(pcm_offset!=0){
      fprintf(stderr,"Non-zero starting PCM offset: %li\n",(long)pcm_offset);
    }
    pcm_print_offset=pcm_offset-48000;
    bitrate=0;
    for(;;){
      ogg_int64_t   next_pcm_offset;
      opus_int16    pcm[120*48*2];
      unsigned char out[120*48*2*2];
      int           li;
      int           si;
      /*Although we would generally prefer to use the float interface, WAV
         files with signed, 16-bit little-endian samples are far more
         universally supported, so that's what we output.*/
      ret=op_read_stereo(of,pcm,sizeof(pcm)/sizeof(*pcm));
      if(ret==OP_HOLE){
        fprintf(stderr,"\nHole detected! Corrupt file segment?\n");
        continue;
      }
      else if(ret<0){
        fprintf(stderr,"\nError decoding '%s': %i\n",_argv1.c_str(),ret);
        if(is_ssl)fprintf(stderr,"Possible truncation attack?\n");
        ret=EXIT_FAILURE;
        break;
      }
      li=op_current_link(of);
      if(li!=prev_li){
        const OpusHead *head;
        const OpusTags *tags;
        int             binary_suffix_len;
        int             ci;
        /*We found a new link.
          Print out some information.*/
        fprintf(stderr,"Decoding link %i:                          \n",li);
        head=op_head(of,li);
        fprintf(stderr,"  Channels: %i\n",head->channel_count);
        if(op_seekable(of)){
          ogg_int64_t duration;
          opus_int64  size;
          duration=op_pcm_total(of,li);
          fprintf(stderr,"  Duration: ");
          print_duration(stderr,duration,3);
          fprintf(stderr," (%li samples @ 48 kHz)\n",(long)duration);
          size=op_raw_total(of,li);
          fprintf(stderr,"  Size: ");
          print_size(stderr,size,0,"");
          fprintf(stderr,"\n");
        }
        if(head->input_sample_rate){
          fprintf(stderr,"  Original sampling rate: %lu Hz\n",
           (unsigned long)head->input_sample_rate);
        }
        tags=op_tags(of,li);
        fprintf(stderr,"  Encoded by: %s\n",tags->vendor);
        for(ci=0;ci<tags->comments;ci++){
          const char *comment;
          comment=tags->user_comments[ci];
          if(opus_tagncompare("METADATA_BLOCK_PICTURE",22,comment)==0){
            OpusPictureTag pic;
            int            err;
            err=opus_picture_tag_parse(&pic,comment);
            fprintf(stderr,"  %.23s",comment);
            if(err>=0){
              fprintf(stderr,"%u|%s|%s|%ux%ux%u",pic.type,pic.mime_type,
               pic.description,pic.width,pic.height,pic.depth);
              if(pic.colors!=0)fprintf(stderr,"/%u",pic.colors);
              if(pic.format==OP_PIC_FORMAT_URL){
                fprintf(stderr,"|%s\n",pic.data);
              }
              else{
                fprintf(stderr,"|<%u bytes of image data>\n",pic.data_length);
              }
              opus_picture_tag_clear(&pic);
            }
            else fprintf(stderr,"<error parsing picture tag>\n");
          }
          else fprintf(stderr,"  %s\n",tags->user_comments[ci]);
        }
        if(opus_tags_get_binary_suffix(tags,&binary_suffix_len)!=NULL){
          fprintf(stderr,"<%u bytes of unknown binary metadata>\n",
           binary_suffix_len);
        }
        fprintf(stderr,"\n");
        if(!op_seekable(of)){
          pcm_offset=op_pcm_tell(of)-ret;
          if(pcm_offset!=0){
            fprintf(stderr,"Non-zero starting PCM offset in link %i: %li\n",
             li,(long)pcm_offset);
          }
        }
      }
      if(li!=prev_li||pcm_offset>=pcm_print_offset+48000){
        opus_int32 next_bitrate;
        opus_int64 raw_offset;
        next_bitrate=op_bitrate_instant(of);
        if(next_bitrate>=0)bitrate=next_bitrate;
        raw_offset=op_raw_tell(of);
        fprintf(stderr,"\r ");
        print_size(stderr,raw_offset,0,"");
        fprintf(stderr,"  ");
        print_duration(stderr,pcm_offset,0);
        fprintf(stderr,"  (");
        print_size(stderr,bitrate,1," ");
        fprintf(stderr,"bps)                    \r");
        pcm_print_offset=pcm_offset;
        fflush(stderr);
      }
      next_pcm_offset=op_pcm_tell(of);
      if(pcm_offset+ret!=next_pcm_offset){
        fprintf(stderr,"\nPCM offset gap! %li+%i!=%li\n",
         (long)pcm_offset,ret,(long)next_pcm_offset);
      }
      pcm_offset=next_pcm_offset;
      if(ret<=0){
        ret=EXIT_SUCCESS;
        break;
      }
      /*Ensure the data is little-endian before writing it out.*/
      for(si=0;si<2*ret;si++){
        out[2*si+0]=(unsigned char)(pcm[si]&0xFF);
        out[2*si+1]=(unsigned char)(pcm[si]>>8&0xFF);
      }
      if(!fwrite(out,sizeof(*out)*4*ret,1,stdout)){
        fprintf(stderr,"\nError writing decoded audio data: %s\n",
         strerror(errno));
        ret=EXIT_FAILURE;
        break;
      }
      nsamples+=ret;
      prev_li=li;
    }
    if(ret==EXIT_SUCCESS){
      fprintf(stderr,"\nDone: played ");
      print_duration(stderr,nsamples,3);
      fprintf(stderr," (%li samples @ 48 kHz).\n",(long)nsamples);
    }
    if(op_seekable(of)&&nsamples!=duration){
      fprintf(stderr,"\nWARNING: "
       "Number of output samples does not match declared file duration.\n");
      if(!output_seekable)fprintf(stderr,"Output WAV file will be corrupt.\n");
    }
    if(output_seekable&&nsamples!=duration){
      make_wav_header(wav_header,nsamples);
      if(fseek(stdout,0,SEEK_SET)||
       !fwrite(wav_header,sizeof(wav_header),1,stdout)){
        fprintf(stderr,"Error rewriting WAV header: %s\n",strerror(errno));
        ret=EXIT_FAILURE;
      }
    }
  }
  op_free(of);
  //return ret;

      auto message = Nan::New<v8::String>("I'm a Node Hero!").ToLocalChecked();
      args.GetReturnValue().Set(message);
    }


    static void Init( Local<Object> exports ) {
      Nan::HandleScope scope;

      Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>( New );
      tpl->SetClassName(Nan::New("OpusEncoder").ToLocalChecked());
      tpl->InstanceTemplate()->SetInternalFieldCount( 1 );

      Nan::SetPrototypeMethod( tpl, "encode", Encode );
      Nan::SetPrototypeMethod( tpl, "decode", Decode );
      Nan::SetPrototypeMethod( tpl, "applyEncoderCTL", ApplyEncoderCTL );
      Nan::SetPrototypeMethod( tpl, "applyDecoderCTL", ApplyDecoderCTL );
      Nan::SetPrototypeMethod( tpl, "setBitrate", SetBitrate );
      Nan::SetPrototypeMethod( tpl, "getBitrate", GetBitrate );
      Nan::SetPrototypeMethod( tpl, "whoAmI", WhoAmI );

      //v8::Persistent<v8::FunctionTemplate> constructor;
      //Nan::AssignPersistent(constructor, tpl);
      exports->Set( Nan::New("OpusEncoder").ToLocalChecked(), tpl->GetFunction() );
    }
};

void NodeInit( Local< Object > exports ) {
  OpusEncoder::Init( exports );
}

NODE_MODULE(node_opus, NodeInit)
