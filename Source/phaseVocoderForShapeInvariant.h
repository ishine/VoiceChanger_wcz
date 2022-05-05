#pragma once
#pragma once
#pragma once

#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <functional>
#include <concepts>

#include "BlockCircularBuffer.h"
#include "Resample.h"

using FloatType = float;

class PhaseVocoder
{
public:
	static constexpr float MaxPitchRatio = 2.0f;
	static constexpr float MinPitchRatio = 0.5f;

	enum class Windows {
		hann,
		hamming,
		kaiser
	};

public:
	PhaseVocoder(int windowLength = 2048, int fftSize = 2048,
		Windows windowType = Windows::hann) :
		samplesTilNextProcess(windowLength),
		windowSize(windowLength),
		resampleBufferSize(windowLength),
		spectralBufferSize(windowLength * 2),
		analysisBuffer(windowLength),
		synthesisBuffer(windowLength * 3),
		windowFunction(windowLength),
		fft(std::make_unique<juce::dsp::FFT>(nearestPower2(fftSize))),
		fftBufferIn(new float[windowLength]),
		fftBufferOut(new float[windowLength]),
		level(new float[spectrumNum])
	{
		windowOverlaps = getOverlapsRequiredForWindowType(windowType);
		analysisHopSize = windowLength / windowOverlaps;
		synthesisHopSize = windowLength / windowOverlaps;

		initialiseWindow(getWindowForEnum(windowType));

		// Processing reuses the spectral buffer to resize the output grain
		// It must be the at least the size of the min pitch ratio
		// TODO FFT size must be big enough also
		spectralBufferSize = windowLength * (1 / MinPitchRatio) < spectralBufferSize ?
			(int)ceil(windowLength * (1 / MinPitchRatio)) : spectralBufferSize;

		spectralBuffer.resize(spectralBufferSize);

		std::fill(spectralBuffer.data(), spectralBuffer.data() + spectralBufferSize, 0.f);

		// Calculate maximium size resample signal can be
		const auto maxResampleSize = (int)std::ceil(std::max(this->windowSize * MaxPitchRatio,
			this->windowSize / MinPitchRatio));

		resampleBuffer.resize(maxResampleSize);
		std::fill(resampleBuffer.data(), resampleBuffer.data() + maxResampleSize, 0.f);
	}

	juce::SpinLock& getParamLock() { return paramLock; }

	int getWindowSize() const { return windowSize; }
	int getLatencyInSamples() const { return windowSize; }
	int getWindowOverlapCount() { return windowOverlaps; }

	float getPitchRatio() const { return pitchRatio; }

	void setPitchRatio(float newRatio)
	{
		pitchRatio = std::clamp(newRatio, PhaseVocoder::MinPitchRatio, PhaseVocoder::MaxPitchRatio);
	}

	float getTimeStretchRatio() const { return timeStretchRatio; }

	int getResampleBufferSize() const { return resampleBufferSize; }

	void updateResampleBufferSize()
	{
		resampleBufferSize = (int)std::ceil(windowSize * analysisHopSize / (float)synthesisHopSize);
		timeStretchRatio = synthesisHopSize / (float)analysisHopSize;
	}

	int getSynthesisHopSize() const { return synthesisHopSize; }

	void setSynthesisHopSize(int hopSize)
	{
		synthesisHopSize = hopSize;
	}

	int getAnalysisHopSize() const { return analysisHopSize; }

	void setAnalysisHopSize(int hopSize)
	{
		analysisHopSize = hopSize;
	}

	const FloatType* const getWindowFunction()
	{
		return windowFunction.data();
	}

	float getRescalingFactor() const { return rescalingFactor; }

	void setRescalingFactor(float factor)
	{
		rescalingFactor = factor;
	}

	// ��Ӧ�㷨
	// 1. ���������������ڲ�����������
	// 2. ������㹻��������ʼ�����ӷ����������ж�ȡһ����
	// 3. ��������ִ�� FFT
	// 4. ��Ƶ��������һЩ����
	// 5. ִ�� iFFT �ص�ʱ��
	// 6. ��������д���ڲ��ϳɻ�����
	// 7. �Ӻϳɻ������ж�ȡһ��������
	void process(FloatType* const audioBuffer, const int audioBufferSize,
		std::function<void(FloatType* const, const int)> processCallback)
	{
		juce::ScopedNoDenormals noDenormals;
		const juce::SpinLock::ScopedLockType lock(paramLock);

		static int callbackCount = 0;
		/////////////////
		//DBG(" ");
		//DBG("Callback: " << ++callbackCount << ", SampleCount: " << incomingSampleCount <<
		//	", (+ incoming): " << audioBufferSize);
		//////////////////
		// �����㹻������д���������������ɴ���
		// ͬ����ֻ��д���㹻�ĺϳɻ�����������
		// ��һ�������Ƶ֡��
		for (auto internalOffset = 0, internalBufferSize = 0;
			internalOffset < audioBufferSize;
			internalOffset += internalBufferSize)
		{
			const auto remainingIncomingSamples = audioBufferSize - internalOffset;
			internalBufferSize = incomingSampleCount + remainingIncomingSamples >= samplesTilNextProcess ?
				samplesTilNextProcess - incomingSampleCount : remainingIncomingSamples;
			/////////////////////////////////
			//DBG("Internal buffer: Offset: " << internalOffset << ", Size: " << internalBufferSize);
			/////////////////////////////////
			jassert(internalBufferSize <= audioBufferSize);

			// Write the incoming samples into the internal buffer
			// Once there are enough samples, perform spectral processing
			const auto previousAnalysisWriteIndex = analysisBuffer.getReadIndex();
			analysisBuffer.write(audioBuffer + internalOffset, internalBufferSize);
			///////////////////////////
			//DBG("Analysis Write Index: " << previousAnalysisWriteIndex << " -> " << analysisBuffer.getWriteIndex());
			///////////////////////////
			incomingSampleCount += internalBufferSize;

			// Collected enough samples, do processing
			if (incomingSampleCount >= samplesTilNextProcess)
			{
				isProcessing = true;


				incomingSampleCount -= samplesTilNextProcess;
				////////////////////////////////////////////////////////
				//DBG(" ");
				//DBG("Process: SampleCount: " << incomingSampleCount);
				////////////////////////////////////////////////////////

				// After first processing, do another process every analysisHopSize samples
				samplesTilNextProcess = analysisHopSize;

				auto spectralBufferData = spectralBuffer.data();

				jassert(spectralBufferSize > windowSize);
				analysisBuffer.setReadHopSize(analysisHopSize);
				analysisBuffer.read(spectralBufferData, windowSize);
				////////////////////////////////////////////
				//DBG("Analysis Read Index: " << analysisBuffer.getReadIndex());
				////////////////////////////////////////////

				// Apply window to signal
				juce::FloatVectorOperations::multiply(spectralBufferData, windowFunction.data(), windowSize);

				// Rotate signal 180 degrees, move the first half to the back and back to the front
				std::rotate(spectralBufferData, spectralBufferData + (windowSize / 2), spectralBufferData + windowSize);

				// Perform FFT, process and inverse FFT
				std::fill(spectralBuffer.begin() + spectralBufferSize / 2, spectralBuffer.end(), 0);
				spectralBuffer.resize(spectralBufferSize * 2);
				fft->performRealOnlyForwardTransform(spectralBufferData);
				
				// copyFromSpectralToFft(spectralBufferData, fftBufferIn);

				processCallback(spectralBufferData, spectralBuffer.size());
				// copyFromSpectralToFft(spectralBufferData, fftBufferIn);
				fft->performRealOnlyInverseTransform(spectralBufferData);
				spectralBuffer.resize(spectralBufferSize);
				// Undo signal back to original rotation
				std::rotate(spectralBufferData, spectralBufferData + (windowSize / 2), spectralBufferData + windowSize);

				// Apply window to signal
				juce::FloatVectorOperations::multiply(spectralBufferData, windowFunction.data(), windowSize);

				// Resample output grain to N * (hop size analysis / hop size synthesis)
				linearResample(spectralBufferData, windowSize, resampleBuffer.data(), resampleBufferSize);
				synthesisBuffer.setWriteHopSize(synthesisHopSize);
				synthesisBuffer.overlapWrite(resampleBuffer.data(), resampleBufferSize);
				////////////////////////////////////////////////////
				//DBG("Synthesis Write Index: " << synthesisBuffer.getWriteIndex());
				////////////////////////////////////////////////////
				setProcessFlag(true);
			}
			// Emit silence until we start producing output
			if (!isProcessing)
			{
				std::fill(audioBuffer + internalOffset, audioBuffer + internalOffset +
					internalBufferSize, 0.f);
				//////////////////////////////////////////
				//DBG("Zeroed output: " << internalOffset << " -> " << internalBufferSize);
				//////////////////////////////////////////
				continue;
			}

			const auto previousSynthesisReadIndex = synthesisBuffer.getReadIndex();
			synthesisBuffer.read(audioBuffer + internalOffset, internalBufferSize);
			//////////////////////////
			//DBG("Synthesis Read Index: " << previousSynthesisReadIndex << " -> " << synthesisBuffer.getReadIndex());
			//////////////////////////

		}

		// Rescale output
		juce::FloatVectorOperations::multiply(audioBuffer, 1.f / rescalingFactor, audioBufferSize);
	}
	void copyFromSpectralToFft(FloatType* spectralBufferData, std::shared_ptr<float> fftBuffer)
	{
		//fftBuffer.reset();
		for (int i = 0, index = 0; index < spectralBufferSize - 1; i++, index += 2)
		{
			fftBuffer.get()[i] = (float)std::sqrtf(spectralBufferData[index] * spectralBufferData[index] + spectralBufferData[index + 1] * spectralBufferData[index + 1]);
		}
	}
	// Principal argument - Unwrap a phase argument to between [-PI, PI]
	static float principalArgument(float arg)
	{
		return std::fmod(arg + juce::MathConstants<FloatType>::pi,
			-juce::MathConstants<FloatType>::twoPi) + juce::MathConstants<FloatType>::pi;
	}

	// Returns the 2^x exponent for a given power of two value
	// If the value given is not a power of two, the nearest power 2 will be used
	static int nearestPower2(int value)
	{
		return (int)log2(juce::nextPowerOfTwo(value));
	}


	std::shared_ptr<float>getSpectrumInput(void)
	{
		auto LevelRange = juce::FloatVectorOperations::findMinAndMax(fftBufferIn.get(), windowSize / 2);
		auto LevelMax = juce::jmax(LevelRange.getEnd(), 200.0f);
		auto LevelMin = juce::jmin(LevelRange.getStart(), 0.0f);
		auto minDB = -60.0f;
		auto maxDB = 0.0f;
		for (int i = 0; i < spectrumNum; i++)
		{
			auto pos = (int)(windowSize / 2) * i / spectrumNum;
			auto data = juce::jmap(fftBufferIn.get()[pos], LevelMin, LevelMax, 0.0f, 1.0f);
			auto power = juce::jmap(
				juce::jlimit(minDB, maxDB, juce::Decibels::gainToDecibels(data)),
				minDB, maxDB, 0.0f, 1.0f
			);
			level.get()[i] = power;
		}
		setProcessFlag(false);

		return level;
	}
	void setProcessFlag(bool flag)
	{
		std::lock_guard<std::mutex> lock(flagLock);
		processDone = flag;
	}
	bool getProcessFlag()
	{
		std::lock_guard<std::mutex> lock(flagLock);
		return processDone;
	}

private:
	using JuceWindow = typename juce::dsp::WindowingFunction<FloatType>;
	using JuceWindowTypes = typename juce::dsp::WindowingFunction<FloatType>::WindowingMethod;

	int getOverlapsRequiredForWindowType(Windows windowType) const
	{
		switch (windowType)
		{
		case Windows::hann:
		case Windows::hamming:
			return 4;

		case Windows::kaiser:
			return 8;

		default:
			return -1;
		}
	}

	JuceWindowTypes getWindowForEnum(Windows windowType)
	{
		switch (windowType)
		{
		case Windows::kaiser:
			return JuceWindow::kaiser;

		case Windows::hamming:
			return JuceWindow::hamming;

		case Windows::hann:
		default:
			return JuceWindow::hann;
		}
	}

	void initialiseWindow(JuceWindowTypes window)
	{
		JuceWindow::fillWindowingTables(windowFunction.data(), windowSize, window, false);
	}

protected:
	std::shared_ptr<float>fftBufferIn, fftBufferOut;
private:
	std::unique_ptr<juce::dsp::FFT> fft;

	// Buffers
	BlockCircularBuffer<FloatType> analysisBuffer;
	BlockCircularBuffer<FloatType> synthesisBuffer;
	std::vector<FloatType> spectralBuffer;
	std::vector<FloatType> resampleBuffer;

	// Misc state
	long incomingSampleCount = 0;
	int spectralBufferSize = 0;
	int samplesTilNextProcess = 0;
	bool isProcessing = false;

	juce::SpinLock paramLock;

	std::mutex flagLock;
	bool processDone{ true };

	std::vector<FloatType> windowFunction;
	float rescalingFactor = 1.f;
	int analysisHopSize = 0;
	int synthesisHopSize = 0;
	int windowSize = 0;
	int resampleBufferSize = 0;
	int windowOverlaps = 0;

	int spectrumNum = 1024;

	float pitchRatio = 0.f;
	float timeStretchRatio = 1.f;
	std::shared_ptr<float> level;
	juce::HeapBlock<juce::dsp::Complex<float>>fftIn, fftOut;
};
