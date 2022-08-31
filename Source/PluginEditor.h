#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

enum FFTOrder
{
    order2048 = 11,
    order4096 = 12,
    order8192 = 13
};

template<typename BlockType>
struct FFTDataGenerator
{
    void produceFFTDataForRendering(const juce::AudioBuffer<float>& audioData, const float negativeInfinity)
    {
        const auto fftSize = getFFTSize();

        fftData.assign(fftData.size(), 0);
        auto* readIndex = audioData.getReadPointer(0);
        std::copy(readIndex, readIndex + fftSize, fftData.begin());

        window->multiplyWithWindowingTable(fftData.data(), fftSize);
        forwardFFT->performFrequencyOnlyForwardTransform(fftData.data());
        int numBins = (int)fftSize / 2;
        
        for (int i = 0; i < numBins; ++i)
        {
            fftData[i] /= (float)numBins;
        }

        for (int i = 0; i < numBins; ++i)
        {
            fftData[i] = juce::Decibels::gainToDecibels(fftData[i], negativeInfinity);
        }

        fftDataFifo.push(fftData);
    }

    void changeOrder(FFTOrder newOrder)
    {
        order = newOrder;
        auto fftSize = getFFTSize();
        forwardFFT = std::make_unique<juce::dsp::FFT>(order);
        window = std::make_unique <juce::dsp::WindowingFunction<float>>(fftSize, juce::dsp::WindowingFunction<float>::blackmanHarris);

        fftData.clear();
        fftData.resize(fftSize * 2, 0);
        fftDataFifo.prepare(fftData.size());
    }
    int getFFTSize() const { return 1 << order; }
    int getNumAvailableFFTDataBlock() const { return fftDataFifo.getNumAvailableForReading(); }
    bool getFFTData(BlockType& fftData) { return fftDataFifo.pull(fftData); }   
        
private:
    FFTOrder order;
    BlockType fftData;
    std::unique_ptr<juce::dsp::FFT> forwardFFT;
    std::unique_ptr<juce::dsp::WindowingFunction<float>> window;

    Fifo<BlockType> fftDataFifo;
};

template<typename PathType>
struct AnalyzerPatthGenerator
{
    void generatePath(const std::vector<float>& renderData,
        juce::Rectangle<float> fftBounds,
        int fftSize,
        float binWidth,
        float negativeInfinity)
    {
        auto top = fftBounds.getY();
        auto bottom = fftBounds.getHeight();
        auto width = fftBounds.getWidth();

        int numBins = (int)fftSize / 2;

        PathType p;
        p.preallocateSpace(3 * (int)fftBounds.getWidth());

        auto map = [bottom, top, negativeInfinity](float v)
        {
            return juce::jmap(v,
                negativeInfinity, 0.f,
                float(bottom), top);
        };

        auto y = map(renderData[0]);
        jassert(!std::isnan(y) && !std::isinf(y));
        p.startNewSubPath(0, y);
        const int pathResolution = 2;
        for (int binNum = 1; binNum < numBins; binNum += pathResolution)
        {
            y = map(renderData[binNum]);
            jassert(!std::isnan(y) && !std::isinf(y));
            if (!std::isnan(y) && !std::isinf(y))
            {
                auto binFreq = binNum * binWidth;
                auto normalizedBInX = juce::mapFromLog10(binFreq, 20.f, 20000.f);
                int binX = std::floor(normalizedBInX * width);
                p.lineTo(binX, y);
            }
        }
        pathFifo.push(p);
    }

    int getNumPathsAvailable() const { return pathFifo.getNumAvailableForReading(); }
    bool getPath(PathType& path) { return pathFifo.pull(path); }

private:
    Fifo<PathType> pathFifo;
};

struct LookAndFeel : juce::LookAndFeel_V4
{
    void drawRotarySlider(juce::Graphics& g,
        int x, int y, int width, int height,
        float sliderPosProportional,
        float rotaryStartAngle,
        float rotaryEndAngle,
        juce::Slider& slider) override;

    void drawToggleButton(juce::Graphics& g, 
        juce::ToggleButton& toggleButton, 
        bool shouldDrawButtonAsHighlighted, 
        bool shouldDrawButtonAsDown) override;
};

struct RotartySliderWithLabels : juce::Slider
{
    RotartySliderWithLabels(juce::RangedAudioParameter& rap, const juce::String& unitSuffix) : 
        juce::Slider(juce::Slider::SliderStyle::RotaryHorizontalVerticalDrag,
        juce::Slider::TextEntryBoxPosition::NoTextBox),
        param(&rap), suffix(unitSuffix)
    {
        setLookAndFeel(&lnf);
    }
    ~RotartySliderWithLabels()
    {
        setLookAndFeel(nullptr);
    }

    struct LabelPos
    {
        float pos;
        juce::String label;
    };

    juce::Array <LabelPos> labels;

    void paint(juce::Graphics& g) override;
    juce::Rectangle<int> getSliderBounds() const;
    int getTextHeight() const { return 14; }
    juce::String getDisplayString() const;

private:
    LookAndFeel lnf;
    juce::RangedAudioParameter* param;
    juce::String suffix;
};

struct PathProducer
{
    PathProducer(SingleChannelSampleFifo<SimpleEQAudioProcessor::BlockType>& scsf) :
        channelFifo(&scsf)
    {
        channelFFTDataGenerator.changeOrder(FFTOrder::order4096);
        monoBuffer.setSize(1, channelFFTDataGenerator.getFFTSize());
    }
    void process(juce::Rectangle<float> fftBounds, double sampleRate);

    juce::Path getPath() { return channelFFTPath; }
private:
    SingleChannelSampleFifo<SimpleEQAudioProcessor::BlockType>* channelFifo;
    juce::AudioBuffer<float> monoBuffer;

    FFTDataGenerator<std::vector<float>> channelFFTDataGenerator;
    AnalyzerPatthGenerator<juce::Path> pathProducer;
    juce::Path channelFFTPath;
};

struct ResponseCurveComponent : juce::Component,
    juce::AudioProcessorParameter::Listener,
    juce::Timer
{
    ResponseCurveComponent(SimpleEQAudioProcessor&);
    ~ResponseCurveComponent();

    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int parameterIndex, bool gestureIsStarting) override {}

    void timerCallback() override;
    void paint(juce::Graphics& g) override;
    void resized() override;

    void toggleAnalysisEnablement(bool enabled)
    {
        shouldShowFFTAnalysis = enabled;
    }
private:    
    SimpleEQAudioProcessor& audioProcessor;
    juce::Atomic<bool> parametersChanged{ false };
    MonoChain monoChain;
    juce::Image background;
    juce::Rectangle<int> getRenderArea();
    juce::Rectangle<int> getAnalysisArea();

    void updateChain();

    PathProducer leftPathProducer, rightPathProducer;

    bool shouldShowFFTAnalysis = true;
};


struct PowerButton : juce::ToggleButton {};
struct AnalyzerButton : juce::ToggleButton 
{
    void resized() override
    {
        auto bounds = getLocalBounds();
        auto insertRect = bounds.reduced(4);
        randomPath.clear();
        juce::Random r;
        randomPath.startNewSubPath(insertRect.getX(), insertRect.getY() + insertRect.getHeight() * r.nextFloat());

        for (auto x = insertRect.getX() + 1; x < insertRect.getRight(); x += 2)
        {
            randomPath.lineTo(x, insertRect.getY() + insertRect.getHeight() * r.nextFloat());
        }
    }
    juce::Path randomPath;
};

//==============================================================================
/**
*/
class SimpleEQAudioProcessorEditor  : public juce::AudioProcessorEditor
{
public:
    SimpleEQAudioProcessorEditor (SimpleEQAudioProcessor&);
    ~SimpleEQAudioProcessorEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;


private:
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    SimpleEQAudioProcessor& audioProcessor;

    RotartySliderWithLabels peakFreqSlider,
                            peakGainSlider,
                            peakQualitySlider, 
                            lowCutFreqSlider, 
                            highCutFreqSlider,
                            lowCutSlopeSlider,
                            highCutSlopeSlider;

    ResponseCurveComponent responseCurveComponent;

    using APVTS = juce::AudioProcessorValueTreeState;
    using Attachment = APVTS::SliderAttachment;

    Attachment              peakFreqSliderAttachment,
                            peakGainSliderAttachment,
                            peakQualitySliderAttachment,
                            lowCutFreqSliderAttachment,
                            highCutFreqSliderAttachment,
                            lowCutSlopeSliderAttachment,
                            highCutSlopeSliderAttachment;

    PowerButton             lowcutBypassButton,
                            peakBypassButton,
                            highcutBypassButton;
    AnalyzerButton          analyzerEnabledButton;

    using ButtonAttachment = APVTS::ButtonAttachment;

    ButtonAttachment        lowcutBypassButtonAttachment,
                            peakBypassButtonAttachment,
                            highcutBypassButtonAttachment,
                            analyzerEnabledButtonAttachment;

    std::vector<juce::Component*> getComps();

    LookAndFeel lnf;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SimpleEQAudioProcessorEditor)
};

namespace MyColors
{
    const juce::Colour Text = { 195u, 195u, 195u };
    const juce::Colour Background = { 46u, 46u, 46u };
    const juce::Colour Foreground = { 50u, 130, 150u };
    const juce::Colour Border = { 175u, 255u, 255u };

}