#pragma once
#include"VoiceConversionImpl.h"
#include"vchsm/convert_C.h"

#include"speex/global_speex_resampler.h"

#include"ppl.h"

#define TDStretch_H
#include"JuceHeader.h"
#include <stddef.h>

#include "FIFOSamplePipeForVC.h"
#include"FIFOBuffer.h"
#define DEFAULT_SEQUENCE_MS         USE_AUTO_SEQUENCE_LEN

#define USE_AUTO_SEQUENCE_LEN       0

#define DEFAULT_SEEKWINDOW_MS       USE_AUTO_SEEKWINDOW_LEN

#define USE_AUTO_SEEKWINDOW_LEN     0

///�ص������Ժ���Ϊ��λ����Ƭ���������л����һ��
///Ϊ���γ������������������������������ĳ���ʱ��
///���������б˴��ص���
///���Ӹ�ֵ�����Ӽ��㸺������֮Ҳ�����ӡ�
#define DEFAULT_OVERLAP_MS      8

    class BufferMatch : public FIFOProcessor
    {
    protected:

        SpeexResamplerState* upResampler;
        SpeexResamplerState* downResampler;
        int err;
        spx_uint32_t spxUpSize;
        spx_uint32_t spxDownSize;

        HSMModel model;
        const char* modelFile{ "D:/Model.dat" };
        
        int channels;
        int sampleReq;

        int overlapLength;
        int seekLength;
        int seekWindowLength;
        int overlapDividerBitsNorm;
        int overlapDividerBitsPure;
        int slopingDivider;
        int sampleRate;
        int sequenceMs;
        int seekWindowMs;
        int overlapMs;

        unsigned long maxnorm;
        float maxnormf;

        double nominalSkip;
        double skipFract;

        bool bQuickSeek;
        bool isBeginning;

        SAMPLETYPE* pMidBuffer;
        SAMPLETYPE* pMidBufferUnaligned;

        FIFOSampleBuffer outputBuffer;
        FIFOSampleBuffer inputBuffer;

        std::vector<SAMPLETYPE> vcOrigBuffer;
        std::vector<SAMPLETYPE> vcConvertedBuffer;
        std::vector<SAMPLETYPE> vcBuffer;
        int bufferLength{ 15000 };
        Eigen::TRowVectorX initializeBuffer;
        Eigen::RowVectorXi pms;
        PicosStructArray picos;
        
        Eigen::TRowVectorX f0s;

		std::unique_ptr<VoiceConversionImpl> pVcImpl;

    	void acceptNewOverlapLength(int newOverlapLength);

        virtual void clearCrossCorrState();
        void calculateOverlapLength(int overlapMs);

        virtual double calcCrossCorr(const SAMPLETYPE* mixingPos, const SAMPLETYPE* compare, double& norm);
        virtual double calcCrossCorrAccumulate(const SAMPLETYPE* mixingPos, const SAMPLETYPE* compare, double& norm);

        virtual int seekBestOverlapPositionFull(const SAMPLETYPE* refPos);
        virtual int seekBestOverlapPositionQuick(const SAMPLETYPE* refPos);
        virtual int seekBestOverlapPosition(const SAMPLETYPE* refPos);

        virtual void overlapMono(SAMPLETYPE* output, const SAMPLETYPE* input) const;

        void clearMidBuffer();
        void overlap(SAMPLETYPE* output, const SAMPLETYPE* input, uint ovlPos) const;

        void calcSeqParameters();
        
        void adaptNormalizer();

        // ʵ�ʵĴ���ӿ�
        void processSamples();

    public:
        BufferMatch(int sampleRate, HSMModel model);
        virtual ~BufferMatch();

        /// �½�һ��ʵ��
        static void* operator new(size_t s, int sampleRate, HSMModel& model);
        /// ����new
        static BufferMatch* newInstance(int sampleRate, HSMModel model);
        /// Returns the output buffer object
        FIFOSamplePipe* getOutput() { return &outputBuffer; };
        ///�������뻺��������
        FIFOSamplePipe* getInput() { return &inputBuffer; };

        /// �趨�µ������ܶ�
        void setTempo(double newTempo);

        /// �����������
        virtual void clear();
        /// ��������
        void clearInput();
        void setChannels(int numChannels);
        /// ʹ�ÿ��ٻ��������ѡ��
        void enableQuickSeek(bool enable);
        /// �Ƿ�������ٻ��������
        bool isQuickSeekEnabled() const;

        /// �������
        void setParameters(int sampleRate,          ///(Hz)
            int sequenceMS = -1,     ///һ�δ������������ (ms)
            int seekwindowMS = -1,   ///Ѱ�����ƥ��ĳ���(ms)
            int overlapMS = -1       ///���е��ӳ��� (ms)
        );
        void setSequenceLength(int sequenceLength);

        ///��ȡ�������
        void getParameters(int* pSampleRate, int* pSequenceMs, int* pSeekWindowMs, int* pOverlapMs) const;

        /// ����������
        virtual void putSamples(
            const SAMPLETYPE* samples,  ///< ������������
            uint numSamples                         ///��ͨ����Ƶ����
        );

        // ���ش����������εı����������Ҫ��
        int getInputSampleReq() const
        {
            return (int)(nominalSkip + 0.5);
        }

        ///���д���һ��ʱ�����������������Ŀ
        int getOutputBatchSize() const
        {
            return seekWindowLength - overlapLength;
        }

        /// ���ؽ��Ƶĳ�ʼ��������ӳ�
        int getLatency() const
        {
            return sampleReq;
        }
    };


