#pragma once
//Ԥ�����߼�����
#define _USE_MATH_DEFINES
#define _OPEN_FILTERS true
#define _OPEN_PEAK_PITCH true
#define _OPEN_WAHWAH false
#define _OPEN_DYNAMICS true
#define _OPEN_TEST false

#if _OPEN_PEAK_PITCH
#define _SHOW_SPEC true
#define USE_3rdPARTYPITCHSHIFT true

#if USE_3rdPARTYPITCHSHIFT
#define USE_RUBBERBAND true
#define USE_SOUNDTOUCH true
#endif
#endif

#include<juce_audio_processors/juce_audio_processors.h>
#include <JuceHeader.h>
#include"juce_audio_plugin_client//Standalone//juce_StandaloneFilterWindow.h"
#include"SoundTouch.h"
#include"PitchShifter.h"
#include"PeakShifter.h"
//#include"PluginFilter.h"
// #include "ManVoiceFilter.h"
//#include "ShapeInvariantPitchShifter.h"
#include"rubberband/RubberBandStretcher.h"
#include"rubberband/rubberband-c.h"
#include"PitchShifterRubberband.h"
#include"PitchShifterSoundTouch.h"
#include"TransportInformation.h"
#include"FIFO.h"
#include"TrainingTemplate.h"
#include"Analyser.h"
#include "EngineHelpers.h"
#include "ExtendedUIBehaviour.h"
#include"SocialButtons.h"
#include"VocoderForVoiceConversion.h"
#include"VoiceConversionBuffer.h"

#include"Utilities.h"
//�������
namespace ParamNames
{
    const juce::String size{ "reverbSize" };
    const juce::String damp{ "reverbDamp" };
    const juce::String width{ "reverbWidth" };
    const juce::String dryWet{ "reverbDw" };
    const juce::String freeze{ "reverbRreeze" };
}
//�ܹ��캯��
class VoiceChanger_wczAudioProcessor :
	 public juce::AudioProcessor//,public juce::AudioAppComponent//,public Filter
    ,public TransportInformation
    ,public juce::AudioProcessorValueTreeState::Listener
	,public juce::ChangeBroadcaster
{

public:
    //�ܿ��߼�
    std::atomic<bool> isModelLoaded{ false }; //�Ƿ��Ѿ�������ģ��
    std::atomic<bool> openReverb{ false };//���쿪��
    std::atomic<bool> isDawStream{ false };//�Ƿ�������������Ƶ��
    std::atomic<bool> isInternalRecording{ false };//�Ƿ�����¼״̬
    std::atomic<bool> useFD{ false };//�Ƿ�ʹ��Ƶ������������
    //�������������
    static void setupInputs(tracktion_engine::Edit& edit)
    {
        auto& dm = edit.engine.getDeviceManager();

        for (int i = 0; i < dm.getNumMidiInDevices(); i++)
        {
            auto dev = dm.getMidiInDevice(i);
            dev->setEnabled(true);
            dev->setEndToEndEnabled(true);
        }

        edit.playInStopEnabled = true;
        edit.getTransport().ensureContextAllocated(true);

        // ��MIDI������ӵ����1
        if (auto t = EngineHelpers::getOrInsertAudioTrackAt(edit, 0))
            if (auto dev = dm.getMidiInDevice(0))
                for (auto instance : edit.getAllInputDevices())
                    if (&instance->getInputDevice() == dev)
                        instance->setTargetTrack(*t, 0, true);

        // ����ͬ��midi������ӵ����2
        if (auto t = EngineHelpers::getOrInsertAudioTrackAt(edit, 1))
            if (auto dev = dm.getMidiInDevice(0))
                for (auto instance : edit.getAllInputDevices())
                    if (&instance->getInputDevice() == dev)
                        instance->setTargetTrack(*t, 0, false);


        edit.restartPlayback();
    }
    //�������������
    static void setupOutputs(tracktion_engine::Edit& edit)
    {
        auto& dm = edit.engine.getDeviceManager();

        for (int i = 0; i < dm.getNumMidiOutDevices(); i++)
        {
            auto dev = dm.getMidiOutDevice(i);
            dev->setEnabled(true);
        }

        edit.playInStopEnabled = true;
        edit.getTransport().ensureContextAllocated(true);

        // Set track 2 to send to midi output
        if (auto t = EngineHelpers::getOrInsertAudioTrackAt(edit, 1))
        {
            auto& output = t->getOutput();
            output.setOutputToDefaultDevice(true);
        }

        edit.restartPlayback();
    }

    //������IO�豸����
    class PluginEngineBehaviour : public tracktion_engine::EngineBehaviour
    {
    public:
        bool autoInitialiseDeviceManager() override { return false; }
    };

    //==============================================================================
    //�༭���������װģ��
    struct EngineWrapper
    {
        EngineWrapper()
            : audioInterface(engine.getDeviceManager().getHostedAudioDeviceInterface())
        {
            JUCE_ASSERT_MESSAGE_THREAD
                audioInterface.initialise({});

            setupInputs(edit);
            setupOutputs(edit);
        }

        tracktion_engine::Engine engine{ ProjectInfo::projectName, std::make_unique<ExtendedUIBehaviour>(), std::make_unique<PluginEngineBehaviour>() };
        tracktion_engine::Edit edit{ engine, tracktion_engine::createEmptyEdit(engine), tracktion_engine::Edit::forEditing, nullptr, 0 };
        tracktion_engine::TransportControl& transport{ edit.getTransport() };
        tracktion_engine::HostedAudioDeviceInterface& audioInterface;
        tracktion_engine::ExternalPlayheadSynchroniser playheadSynchroniser{ edit };
    };

    template<typename Function>
    void callFunctionOnMessageThread(Function&& func)
    {
        if (MessageManager::getInstance()->isThisTheMessageThread())
        {
            func();
        }
        else
        {
            jassert(!MessageManager::getInstance()->currentThreadHasLockedMessageManager());
            WaitableEvent finishedSignal;
            MessageManager::callAsync([&]
                {
                    func();
                    finishedSignal.signal();
                });
            finishedSignal.wait(-1);
        }
    }

    void ensureEngineCreatedOnMessageThread()
    {
        if (!engineWrapper)
            callFunctionOnMessageThread([&] { engineWrapper = std::make_unique<EngineWrapper>(); });
    }

    void ensurePrepareToPlayCalledOnMessageThread(double sampleRate, int expectedBlockSize)
    {
        jassert(engineWrapper);
        callFunctionOnMessageThread([&] { engineWrapper->audioInterface.prepareToPlay(sampleRate, expectedBlockSize); });
    }

    std::unique_ptr<EngineWrapper> engineWrapper;



//====================================================================================

public:

    //��¼ģ��
    void stop();
    CriticalSection modelWriterLock;
    CriticalSection writerLock;
    std::atomic<bool> openVoiceConversion{ false };
    // bool isInternalRecording{ false };
    TimeSliceThread internalWriteThread;
    std::unique_ptr<AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<AudioFormatWriter::ThreadedWriter*> activeWriter{ nullptr };
    File parentDir = File::getSpecialLocation(File::userDesktopDirectory);
    File lastRecording = parentDir.getNonexistentChildFile("VoiceChanger_wcz Recording", ".wav");

	//ScopedPointer<FileOutputStream> internalRecordingStream;
    //ScopedPointer<AudioFormatWriter> internalRecordWriter;
    // ScopedPointer<AudioFormatWriter::ThreadedWriter> threadedInternalRecording;
    void startRecording(const File& file);
    void stopRecording();

    //=============================================================================================
    //������ģ�����
	enum FilterType
    {
        NoFilter = 0,
        HighPass,
        HighPass1st,
        LowShelf,
        BandPass,
        AllPass,
        AllPass1st,
        Notch,
        Peak,
        HighShelf,
        LowPass1st,
        LowPass,
        LastFilterID
    };
    // ������������
    static juce::String paramOutput;
    static juce::String paramType;
    static juce::String paramFrequency;
    static juce::String paramQuality;
    static juce::String paramGain;
    static juce::String paramActive;
    // ��ȡ��������
    static juce::String getBandID(size_t index);
    static juce::String getTypeParamName(size_t index);
    static juce::String getFrequencyParamName(size_t index);
    static juce::String getQualityParamName(size_t index);
    static juce::String getGainParamName(size_t index);
    static juce::String getActiveParamName(size_t index);
public:
    //==============================================================================
    //�ܹ��캯��������Ϊjuce��װ�ӿڴ���
    VoiceChanger_wczAudioProcessor();
    ~VoiceChanger_wczAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif
    // �ܴ���ӿ�
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void overallProcess(juce::AudioBuffer<float>& buffer);
    // �������仯��Ӧ
    void parameterChanged(const juce::String& parameterID, float newValue) override;
    // ��ȡ������״̬
    juce::AudioProcessorValueTreeState& getPluginState();

    // ��ȡ���������˲�������
    size_t getNumBands() const;
    juce::String getBandName(size_t index) const;
    juce::Colour getBandColour(size_t index) const;

    void setBandSolo(int index);
    bool getBandSolo(int index) const;
    
    static juce::StringArray getFilterTypeNames();
    // ���������༭���ӿ�
	juce::AudioProcessorEditor* createEditor() override;

    // ����true
	bool hasEditor() const override;
    // ��ȡ��Ƶ��Ӧ�����������
    const std::vector<double>& getMagnitudes();
    // ����Ƶ�׻�ͼ
    void createFrequencyPlot(juce::Path& p, const std::vector<double>& mags, const juce::Rectangle<int> bounds, float pixelsPerDouble);
    // �����˲���Ƶ���ͼ
    void createAnalyserPlot(juce::Path& p, const juce::Rectangle<int> bounds, float minFreq, bool input);
    // ����Ƿ����µ�Ƶ�����
    bool checkForNewAnalyserData();

    //=========================================================
    //JUCEϵͳ�ӿ�
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;


    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;


    void  getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;
    //==========================================================
    juce::Point<int> getSavedSize() const;
    //�༭����������
    void setSavedSize(const juce::Point<int>& size);

    // ��ȡ������Ƶ��
    juce::Image& getSpectrumView();
    // �����������ų̶�
    void setPitchShift(float pitch);
    //���ù�����ƶ��̶�
    void setPeakShift(float peak);
    
    //���ö�̬���ڲ���
    void setDynamicsThresholdShift(float threshold);
    void setDynamicsRatioShift(float ratio);
    void setDynamicsAttackShift(float attack);
    void setDynamicsReleaseShift(float release);
    void setDynamicsMakeupGainShift(float makeupGain);

    // ����������Ƶ����λ��
    void setPlayAudioFilePosition(float position);
    // ��ȡ�������Ų���
    float getPitchShift();
    //��ȡ������ƶ�����
    float getPeakShift();

    //��ȡ��̬�������
    float getDynamicsThresholdShift();
    float getDynamicsRatioShift();
    float getDynamicsAttackShift();
    float getDynamicsReleaseShift();
    float getDynamicsMakeupGainShift();
    // ����������
    //==============================================================================
    struct Band {
        Band(const juce::String& nameToUse, juce::Colour colourToUse, FilterType typeToUse,
            float frequencyToUse, float qualityToUse, float gainToUse = 1.0f, bool shouldBeActive = true)
            : name(nameToUse),
            colour(colourToUse),
            type(typeToUse),
            frequency(frequencyToUse),
            quality(qualityToUse),
            gain(gainToUse),
            active(shouldBeActive)
        {}
        // �����˲���Ӧ�еĲ���
        juce::String name;
        juce::Colour colour;
        FilterType   type = BandPass;
        float        frequency = 1000.0f;
        float        quality = 1.0f;
        float        gain = 1.0f;
        bool         active = true;
        std::vector<double> magnitudes;
    };
    // ��ȡʵ��ָ��
    Band* getBand(size_t index);
    // ��ȡ�����˲����ǵڼ���
    int getBandIndexFromID(juce::String paramID);


private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VoiceChanger_wczAudioProcessor);
    // ����IIR�˲�������
    void updateBand(const size_t index);
    // ������ͨ״̬
    void updateBypassedStates();
    // ���·�Ƶ��Ӧ����
    void updatePlots();
    // ���ز���
    juce::UndoManager                  undo;


    juce::AudioProcessorValueTreeState state;
#if _OPEN_FILTERS
    std::vector<Band>    bands; // �������bandʵ��

    std::vector<double> frequencies; // �����˲���Ƶ�ʲ���
    std::vector<double> magnitudes; // �˲������Ȳ���

    bool wasBypassed = true; //�Ƿ���ͨ
    using FilterBand = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>>;
    using Gain = juce::dsp::Gain<float>;
    // ������DSP��������
    juce::dsp::ProcessorChain<FilterBand, FilterBand, FilterBand, FilterBand, FilterBand, FilterBand, Gain> filter;
    


    double sampleRate = 44100;

    int soloed = -1;

    Analyser<float> inputAnalyser; // ����Ƶ�׷�����
    Analyser<float> outputAnalyser; //���Ƶ�׷�����


    juce::Point<int> editorSize = { 900, 500 }; // Ĭ�Ͼ����������С
#endif

    //================================================================================
public:
    //��̬���������
#if _OPEN_DYNAMICS 
    juce::AudioBuffer<float> mixedDownInputDynamics;
    float xlDynamics;
    float ylDynamics;
    float xgDynamics;
    float ygDynamics;
    float controlDynamics;
    float inputLevelDynamics;
    float ylPrevDynamics;
    float inverseEDynamics;
    float inverseSampleRateDynamics;
    float calculateAttackOrReleaseForDynamics(float value);
    void processDynamics(
        juce::AudioBuffer<float>& buffer
        ,bool isExpanderOrCompressor
        ,float threshold
        ,float ratio
        ,float attack
        ,float release
        ,float makeupGain
    );
#endif


public:
    int nPlayAudioFilePosition{ 0 };
    int nPlayAudioFileSampleNum{ 0 };
    bool realtimeMode{ true }; // ����ģʽ/ʵʱģʽ�л�

    juce::AudioFormatManager formatManager;// ���ڽ�MP3/wav�����ļ�ת������Ƶ��

    juce::AudioTransportSource transportSource; // ���ڿ���������������
    File currentlyLoadedFile; // ��ǰ������ļ�
    AudioThumbnailCache thumbnailCache; //����������Ƶ����ͼ
    ScopedPointer<AudioFormatReaderSource>currentAudioFileSource;//��Ƶ�ļ���ȡ��
    TimeSliceThread readAheadThread;//�����ļ��ĳ�ǰ��ȡ�߳�
    void loadFileIntoTransport(const File& audioFile);//���ļ����벥�ſ���ģ��


    juce::AudioBuffer<float> fileBuffer;
    int innerRecordSampleCount{ 0 };
    juce::AudioBuffer<float> sourceBuffer;//�洢Դ����
    juce::AudioBuffer<float> targetBuffer;//�洢Ŀ������
    
    HSMModel model;
    // TrainingTemplate trainingTemplate;
    // std::vector<float> voiceChangerParameter;
    juce::AudioBuffer<float>* pPlayBuffer;
    TransportInformation ti;
    void setTarget(TransportFileType ft)override;
    void setState(TransportState newState)override;
    float inputAudioFileLength{ 300.0f };
    void getNextAudioBlock(juce::AudioSourceChannelInfo& buffer);
    // int readFilePosition;
    bool shouldProcessFile{ false };
    bool canReadSampleBuffer{ false };
    //void alignBuffer(juce::AudioSampleBuffer& s, juce::AudioSampleBuffer& t);
    float getPlayAudioFilePosition();


#if _OPEN_WAHWAH//WAHWAHЧ��������
    juce::OwnedArray<Filter> filtersForWahWah;
    void processWahwah(juce::AudioBuffer<float>& buffer,
        float attackValue,
        float releaseValue,
        float maxLFOAndEnvelope,
        float lfoFrequency,
        float mixRatio,
        float filterQFactor,
        float filterGain,
        float filterFreq
    );
    float centerFrequencyForWahWah;
    float lfoPhaseForWahWah;
    float inverseSampleRateForWahWah;
    float twoPi;

    juce::Array<float> envelopesForWahWah;
    float inverseEForWahWah;
    float calculateAttackOrReleaseForWahWah(float value);

#endif
#if _OPEN_PEAK_PITCH
    juce::OwnedArray<PitchShifter>pitchShifters;// ����������ĵ���
    juce::OwnedArray<PeakShifter>peakShifters; //������ƶ���ĵ���


	//juce::OwnedArray<VocoderForVoiceConversion> vocodersForVoiceConversion;



#endif
#if _OPEN_TEST
    juce::OwnedArray<ShapeInvariantPitchShifter> shapeInvariantPitchShifters;
#endif
    void updateUIControls();

private:
    // �������Ų���
    juce::AudioParameterFloat* nPitchShift{ 0 };
    juce::AudioParameterFloat* nPeakShift;
    // ��̬�������
    juce::AudioParameterFloat* nDynamicsThreshold;
    juce::AudioParameterFloat* nDynamicsRatio;
    juce::AudioParameterFloat* nDynamicsAttack;
    juce::AudioParameterFloat* nDynamicsRelease;
    juce::AudioParameterFloat* nDynamicsMakeupGain;

    // ����Ƶ������
    void drawSpectrumGraph(juce::Image view, std::shared_ptr<float>level, juce::Colour colour, bool isLog);
    //void syncPluginParameter();
public:
    juce::Image spectrum;// Ƶ�׻滭����
private:
#if USE_3rdPARTYPITCHSHIFT
#if USE_RUBBERBAND
    std::unique_ptr<PitchShifterRubberband> rbs;// Ƶ������ýӿ�
    const int rbOptions = RubberBand::RubberBandStretcher::Option::OptionProcessRealTime + RubberBand::RubberBandStretcher::Option::OptionPitchHighConsistency;
#endif // USE_3rdPARTYPITCHSHIFT
#if USE_SOUNDTOUCH
    std::unique_ptr<PitchShifterSoundTouch> sts;//soundtouch���ýӿ�
#endif
#endif
public:
    std::unique_ptr<VoiceConversionBuffer> vcb;// ��ģ��������ýӿ�
    int samplesPerBlock;
private:
    //��ƽ��ģ�����
    juce::LinearSmoothedValue<float> gainLeft, gainRight;//������������������
    std::vector<juce::LinearSmoothedValue<float>>rmsLevels;//��������ƽ
    Utility::Fifo rmsFifo;
    juce::AudioBuffer<float>rmsCalculationBuffer;//����

    int rmsWindowSize = 50;

    int isSmoothed = true;
public:
    //��ƽ��ģ�鷽��
    // juce::AudioProcessorValueTreeState& getApvts() { return parameters; }
    std::vector<float>getRmsLevels();
    float getRmsLevel(const int level);
    void processLevelValue(juce::LinearSmoothedValue<float>&, const float value)const;
    void processLevelInfo(juce::AudioBuffer<float>& buffer);
    //==============================================================================
    //JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoiceChanger_wczAudioProcessor)
private:
    //���»�������
    void updateReverbSettings();
    //��������洢
    dsp::Reverb::Parameters reverbParams;
    dsp::Reverb reverb;
};

