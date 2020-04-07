// TestPlayer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <iostream>
#include <tuple>
using namespace std;

#define _STDC_CONSTANT_MACROS
#define SDL_MAIN_HANDLED
extern "C" {
#include "SDL.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/imgutils.h"
#include "libswresample/swresample.h"
#include "libswscale/swscale.h"
};

int audioIndex = -1;

//存储音频信息结构体（主要用于输入音频和输出音频的信息保存）
struct AudioInfo {
  int64_t layout;
  int sampleRate;
  int channels;
  AVSampleFormat format;

  AudioInfo() {
    layout = -1;
    sampleRate = -1;
    channels = -1;
    format = AV_SAMPLE_FMT_S16;
  }

  AudioInfo(int64_t l, int rate, int c, AVSampleFormat f)
      : layout(l), sampleRate(rate), channels(c), format(f) {}
};

struct UserData {
  AVFormatContext* pFormatCtx;
  AVCodecContext* pCodecCtx;
  AudioInfo inn;
  AudioInfo outt;
};

//用于申请数据的存储空间
int allocDataBuf(AudioInfo in, AudioInfo out, uint8_t** outData,
                 int inputSamples) {
  int bytePerOutSample = -1;
  switch (out.format) {
    case AV_SAMPLE_FMT_U8:
      bytePerOutSample = 1;
      break;
    case AV_SAMPLE_FMT_S16P:
    case AV_SAMPLE_FMT_S16:
      bytePerOutSample = 2;
      break;
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_S32P:
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_FLTP:
      bytePerOutSample = 4;
      break;
    case AV_SAMPLE_FMT_DBL:
    case AV_SAMPLE_FMT_DBLP:
    case AV_SAMPLE_FMT_S64:
    case AV_SAMPLE_FMT_S64P:
      bytePerOutSample = 8;
      break;
    default:
      bytePerOutSample = 2;
      break;
  }

  int guessOutSamplesPerChannel =
      av_rescale_rnd(inputSamples, out.sampleRate, in.sampleRate, AV_ROUND_UP);
  int guessOutSize =
      guessOutSamplesPerChannel * out.channels * bytePerOutSample;

  std::cout << "GuessOutSamplesPerChannel: " << guessOutSamplesPerChannel
            << std::endl;
  std::cout << "GuessOutSize: " << guessOutSize << std::endl;

  guessOutSize *= 1.2;  // just make sure.

  *outData = (uint8_t*)av_malloc(sizeof(uint8_t) * guessOutSize);
  // av_samples_alloc(&outData, NULL, outChannels, guessOutSamplesPerChannel,
  // AV_SAMPLE_FMT_S16, 0);
  return guessOutSize;
}

//将ffmpeg抓取到的音频frame存储至databuffer中
tuple<int, int> reSample(AudioInfo in, AudioInfo out, uint8_t* dataBuffer,
                         int dataBufferSize, const AVFrame* frame) {
  SwrContext* swr =
      swr_alloc_set_opts(nullptr, out.layout, out.format, out.sampleRate,
                         in.layout, in.format, in.sampleRate, 0, nullptr);
  cout << out.layout << "," << out.format << "," << out.sampleRate << ","
       << in.layout << "," << in.format << "," << in.sampleRate << endl;
  if (swr_init(swr)) {
    cout << "swr_init error." << endl;
    throw std::runtime_error("swr_init error.");
  }
  int outSamples =
      swr_convert(swr, &dataBuffer, dataBufferSize,
                  (const uint8_t**)&frame->data[0], frame->nb_samples);
  cout << "reSample: nb_samples=" << frame->nb_samples
       << ", sample_rate = " << frame->sample_rate
       << ", outSamples=" << outSamples << endl;
  if (outSamples <= 0) {
    throw std::runtime_error("error: outSamples=" + outSamples);
  }

  int outDataSize =
      av_samples_get_buffer_size(NULL, out.channels, outSamples, out.format, 1);

  if (outDataSize <= 0) {
    throw std::runtime_error("error: outDataSize=" + outDataSize);
  }
  return {outSamples, outDataSize};
}

void audio_callback(void* userdata, Uint8* stream, int len) {
  UserData* udata = (UserData*)userdata;

  static AVFrame* aframe = av_frame_alloc();  //存储音频帧;
  static AVPacket packet;
  av_init_packet(&packet);
  AVFormatContext* pFormatCtx = udata->pFormatCtx;
  AVCodecContext* pCodecCtx = udata->pCodecCtx;
  while (1) {
    //只读取音频帧
    if (av_read_frame(pFormatCtx, &packet) < 0) {
      cout << "can't read packet" << endl;
      throw std::runtime_error("can't read packet");
    }
    if (packet.stream_index == audioIndex) break;
  }
  if (avcodec_send_packet(pCodecCtx, &packet) < 0) {
    cout << "send packet error" << endl;
    throw std::runtime_error("can't send packet");
  }
  if (avcodec_receive_frame(pCodecCtx, aframe) < 0) {
    cout << "can't get frame" << endl;
    throw std::runtime_error("can't get frame");
  }
  cout << "frame " << aframe->nb_samples << " get" << endl;
  static uint8_t* outBuffer = nullptr;
  static int outBufferSize = 0;

  if (outBuffer == nullptr) {
    outBufferSize =
        allocDataBuf(udata->inn, udata->outt, &outBuffer, aframe->nb_samples);
    cout << " --------- audio samples: " << aframe->nb_samples << endl;
  } else {
    memset(outBuffer, 0, outBufferSize);
  }

  int outSamples;
  int outDataSize;
  std::tie(outSamples, outDataSize) =
      reSample(udata->inn, udata->outt, outBuffer, outBufferSize, aframe);

  if (outDataSize != len) {
    cout << "WARNING: outDataSize[" << outDataSize << "] != len[" << len << "]"
         << endl;
  }

  std::memcpy(stream, outBuffer, outDataSize);
}
int main() {
  AVFormatContext* pFormatCtx;
  int i;
  AVCodecParameters* parser;
  AVCodecContext* pCodecCtx;
  AVCodec* pCodec;
  uint8_t* out_buffer;

  //输入文件路径
  char filepath[] = "D:/c++workspace/VideoPlayer/Titanic.ts";

  int frame_cnt;

  avformat_network_init();  //加载socket库以及网络加密协议相关的库，为后续使用网络相关提供支持
  pFormatCtx = avformat_alloc_context();  //封装格式上下文结构体

  // start init AVCodecContext
  if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) !=
      0) {  //打开输入视频文件
    printf("Couldn't open input stream.\n");
    return -1;
  }
  if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {  //获取视频文件信息
    printf("Couldn't find stream information.\n");
    return -1;
  }
  for (i = 0; i < pFormatCtx->nb_streams; i++)
    if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
      audioIndex = i;
      break;
    }
  if (audioIndex == -1) {
    printf("Didn't find a audio stream.\n");
    return -1;
  }

  parser = pFormatCtx->streams[audioIndex]->codecpar;

  pCodec = avcodec_find_decoder(parser->codec_id);  //视频编解码器
  if (pCodec == NULL) {
    printf("Codec not found.\n");
    return -1;
  }
  pCodecCtx = avcodec_alloc_context3(pCodec);
  if (!pCodecCtx) {
    printf("Could not allocate video codec context\n");
    return -1;
  }
  if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {  //打开解码器
    printf("Could not open codec.\n");
    return -1;
  }
  // init AVCodecContext over

  // start sdl audio
  //音频信息获取
  int64_t inLayout = parser->channel_layout;
  int inChannels = parser->channels;
  int inSampleRate = parser->sample_rate;
  AVSampleFormat inFormate = AVSampleFormat(pCodecCtx->sample_fmt);

  SDL_AudioSpec wanted_spec;
  SDL_AudioSpec specs;
  UserData udata;
  udata.pCodecCtx = pCodecCtx;
  udata.pFormatCtx = pFormatCtx;
  udata.inn = AudioInfo(inLayout, inSampleRate, inChannels, inFormate);
  udata.outt =
      AudioInfo(AV_CH_LAYOUT_STEREO, inSampleRate, 2, AV_SAMPLE_FMT_S16);

  wanted_spec.freq = parser->sample_rate;
  wanted_spec.format = AUDIO_S16SYS;
  wanted_spec.channels = parser->channels;
  wanted_spec.samples = 1024;  // set by output samples
  wanted_spec.callback = audio_callback;
  wanted_spec.userdata = &udata;
  SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE", "1", 1);

  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
    string errMsg = "Could not initialize SDL -";
    errMsg += SDL_GetError();
    cout << errMsg << endl;
    throw std::runtime_error(errMsg);
  }

  SDL_AudioDeviceID audioDeviceId =
      SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &specs, 0);  //[1]

  if (audioDeviceId == 0) {
    cout << "Failed to open audio device:" << SDL_GetError() << endl;
    return -1;
  }
  cout << "wanted_specs.freq:" << wanted_spec.freq << endl;
  // cout << "wanted_specs.format:" << wanted_specs.format << endl;
  std::printf("wanted_specs.format: Ox%X\n", wanted_spec.format);
  cout << "wanted_specs.channels:" << (int)wanted_spec.channels << endl;
  cout << "wanted_specs.samples:" << (int)wanted_spec.samples << endl;

  cout << "------------------------------------------------" << endl;
  cout << "specs.freq:" << specs.freq << endl;
  // cout << "specs.format:" << specs.format << endl;
  std::printf("specs.format: Ox%X\n", specs.format);
  cout << "specs.channels:" << (int)specs.channels << endl;
  cout << "specs.silence:" << (int)specs.silence << endl;
  cout << "specs.samples:" << (int)specs.samples << endl;

  cout << "waiting audio play..." << endl;

  SDL_PauseAudioDevice(audioDeviceId, 0);  // [2]

  SDL_Delay(300000);

  SDL_CloseAudio();

  //----------------------------------

  return 0;
}

// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧:
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5.
//   转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件
