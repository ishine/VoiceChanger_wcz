

#pragma once
#include <JuceHeader.h>

namespace EngineHelpers
{
    tracktion_engine::Project::Ptr createTempProject(tracktion_engine::Engine& engine);
    void showAudioDeviceSettings(tracktion_engine::Engine& engine); ///< ��ȡ��Ƶ�豸����
    void browseForAudioFile(tracktion_engine::Engine& engine, std::function<void(const juce::File&)> fileChosenCallback);
    void removeAllClips(tracktion_engine::AudioTrack& track);
    tracktion_engine::AudioTrack* getOrInsertAudioTrackAt(tracktion_engine::Edit& edit, int index);
    tracktion_engine::WaveAudioClip::Ptr loadAudioFileAsClip(tracktion_engine::Edit& edit, const juce::File& file);
    template<typename ClipType> typename ClipType::Ptr loopAroundClip(ClipType& clip);
    void stop(tracktion_engine::Edit& edit); ///ֹͣ������
    void togglePlay(tracktion_engine::Edit& edit); ///< �����༭������
    void toggleRecord(tracktion_engine::Edit& edit); ///< �����ǰ�������¼��������¼��
    void armRecordTrack(tracktion_engine::AudioTrack& t, bool arm, int position = 0); /// ���ù���Ƿ����¼��
    bool isTrackArmed(tracktion_engine::AudioTrack& t, int position = 0);
    bool isInputMonitoringEnabled(tracktion_engine::AudioTrack& t, int position = 0);
    void enableInputMonitoring(tracktion_engine::AudioTrack& t, bool im, int position = 0);
    bool trackHasInput(tracktion_engine::AudioTrack& t, int position = 0);
    std::unique_ptr<juce::KnownPluginList::PluginTree> createPluginTree(tracktion_engine::Engine& engine);
}
