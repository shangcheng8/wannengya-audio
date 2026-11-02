#include "CaptureEditor.h"

#include "SystemCompat.h"

#include "AgentLauncher.h"

#include <BinaryData.h>

using namespace wng_assets;

#include <cmath>

using namespace wng;
using namespace wng::theme;

namespace {

juce::Image toImage(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty())
        return {};

    return juce::ImageFileFormat::loadFrom(bytes.data(), bytes.size());
}

// One column grid, no nested boxes: every control starts at kFieldX and ends at
// kWidth - kMargin, and the meters use the same two edges, so nothing can drift
// out of line when a row is added.
constexpr int kWidth = 520;
constexpr int kHeight = 420;

constexpr int kMargin = 24;
constexpr int kLabelColumn = 100;
constexpr int kLabelGap = 12;
constexpr int kFieldX = kMargin + kLabelColumn + kLabelGap;
constexpr int kFieldWidth = kWidth - kMargin - kFieldX;
constexpr int kFieldHeight = 44;

constexpr int kHeaderTop = 26;
constexpr int kRuleY = 96;
constexpr int kAppRowY = 114;
constexpr int kPairRowY = kAppRowY + kFieldHeight + 12;
constexpr int kMeterRuleY = kPairRowY + kFieldHeight + 22;
constexpr int kMeterY = kMeterRuleY + 24;
constexpr int kMeterRowHeight = 34;
constexpr int kMeterBarHeight = 10;
constexpr int kFooterY = kMeterY + kMeterRowHeight * 3 + 18;

} // namespace

CaptureEditor::CaptureEditor(CaptureProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setLookAndFeel(&editLookAndFeel());

    setOpaque(true);
    setSize(kWidth, kHeight);

    // ---- header -------------------------------------------------------------
    logo = juce::ImageFileFormat::loadFrom(wng_assets::logo_png, wng_assets::logo_pngSize);

    title.setText(zh("万能鸭录音机架"), juce::dontSendNotification);
    title.setFont(font(24.0f, 0.62f));
    title.setColour(juce::Label::textColourId, textPrimary);
    addAndMakeVisible(title);

    subtitle.setText("Capture", juce::dontSendNotification);
    subtitle.setFont(font(12.5f));
    subtitle.setColour(juce::Label::textColourId, textTertiary);
    addAndMakeVisible(subtitle);

    addAndMakeVisible(agentDot);

    agentLabel.setFont(font(13.5f));
    agentLabel.setColour(juce::Label::textColourId, textSecondary);
    agentLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(agentLabel);

    // ---- app picker ---------------------------------------------------------
    appCaption.setText(zh("应用"), juce::dontSendNotification);
    appCaption.setFont(font(20.0f));
    appCaption.setColour(juce::Label::textColourId, textSecondary);
    addAndMakeVisible(appCaption);

    appBox.setLookAndFeel(&editLookAndFeel());

    // Per-App capture is a process tap, which only exists from macOS 14.2. On
    // older systems the picker stays visible but inert, and says why.
    if (! wng::systemSupportsAppCapture())
    {
        appBox.setEnabled(false);
        appBox.setTextWhenNothingSelected(zh("需要 macOS 14.2+"));
        appBox.setTooltip(zh("应用采集需要 macOS 14.2 或更高，当前系统是 ")
                          + juce::String(wng::systemVersionText()) + zh("。可以改用虚拟通道。"));
    }
    else
    {
        appBox.setTextWhenNothingSelected(zh("选择应用"));
    }
    appBox.onChange = [this]
    {
        const int id = appBox.getSelectedId();

        if (id <= 1)
        {
            processor.clearApp();
            updateStatus();
            return;
        }

        const int index = id - 2;

        if (index < 0 || index >= static_cast<int>(processes.size()))
            return;

        const auto& info = processes[static_cast<size_t>(index)];
        processor.selectApp(info.pid, juce::String::fromUTF8(info.bundleID.c_str()),
                            juce::String::fromUTF8(info.name.c_str()));
        updateStatus();
    };
    addAndMakeVisible(appBox);

    refreshButton.setLookAndFeel(&buttonLookAndFeel());
    refreshButton.setButtonText(zh("刷新"));
    refreshButton.setTooltip(zh("重新扫描应用列表"));
    refreshButton.onClick = [this]
    {
        refreshApps();
        processor.reconnect();
        updateStatus();
    };
    addAndMakeVisible(refreshButton);

    // ---- pair picker --------------------------------------------------------
    pairCaption.setText(zh("虚拟通道"), juce::dontSendNotification);
    pairCaption.setFont(font(20.0f));
    pairCaption.setColour(juce::Label::textColourId, textSecondary);
    addAndMakeVisible(pairCaption);

    pairBox.setLookAndFeel(&editLookAndFeel());
    pairBox.onChange = [this]
    {
        const int id = pairBox.getSelectedId();

        if (id == 1)
        {
            processor.setUsePair(false);
        }
        else if (id >= 2)
        {
            processor.selectPair(id - 2);
        }

        updateStatus();
    };
    addAndMakeVisible(pairBox);

    for (int i = 0; i < kDevicePairs; ++i)
        pairBox.addItem(kPairName[i], i + 2);

    pairBox.addItem(zh("不使用"), 1);

    // ---- meters -------------------------------------------------------------
    auto setupMeter = [this](LevelMeter& meter, juce::Label& label, const char* text)
    {
        addAndMakeVisible(meter);
        label.setText(zh(text), juce::dontSendNotification);
        label.setFont(font(13.5f));
        label.setColour(juce::Label::textColourId, textTertiary);
        label.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(label);
    };

    setupMeter(appMeter, appMeterLabel, "应用");
    setupMeter(pairMeter, pairMeterLabel, "通道");
    setupMeter(outMeter, outMeterLabel, "输出");

    // ---- footer -------------------------------------------------------------
    footer.setFont(font(13.5f));
    footer.setColour(juce::Label::textColourId, textSecondary);
    footer.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(footer);

    refreshApps();
    syncFromProcessor();
    updateStatus();

    startTimer(120);
}

CaptureEditor::~CaptureEditor()
{
    stopTimer();

    appBox.setLookAndFeel(nullptr);
    pairBox.setLookAndFeel(nullptr);
    refreshButton.setLookAndFeel(nullptr);
    setLookAndFeel(nullptr);
}

//==================================================================================================
#pragma mark Layout
//==================================================================================================

void CaptureEditor::paint(juce::Graphics& g)
{
    // One flat surface with a barely-there vertical lift; the grouping is done
    // with spacing and two hairlines, not with boxes inside boxes.
    g.setGradientFill(juce::ColourGradient(background.brighter(0.045f), 0.0f, 0.0f,
                                           background, 0.0f, static_cast<float>(getHeight()),
                                           false));
    g.fillRect(getLocalBounds());

    if (logo.isValid())
        g.drawImage(logo, juce::Rectangle<float>(static_cast<float>(kMargin),
                                                 static_cast<float>(kHeaderTop + 2), 40.0f, 40.0f),
                    juce::RectanglePlacement::centred, false);

    paintHairline(g, kMargin, kWidth - kMargin, kRuleY);
    paintHairline(g, kMargin, kWidth - kMargin, kMeterRuleY);
}

void CaptureEditor::resized()
{
    // ---- header -------------------------------------------------------------
    const int textX = kMargin + 40 + 14;

    title.setBounds(textX, kHeaderTop, 300, 30);
    subtitle.setBounds(textX, kHeaderTop + 28, 300, 16);

    agentLabel.setBounds(kWidth - kMargin - 158, kHeaderTop + 14, 138, 20);
    agentDot.setBounds(kWidth - kMargin - 14, kHeaderTop + 17, 12, 12);

    // ---- source rows --------------------------------------------------------
    appCaption.setBounds(kMargin, kAppRowY, kLabelColumn, kFieldHeight);

    constexpr int refreshWidth = 56;
    refreshButton.setBounds(kFieldX + kFieldWidth - refreshWidth, kAppRowY,
                            refreshWidth, kFieldHeight);
    appBox.setBounds(kFieldX, kAppRowY, kFieldWidth - refreshWidth - 10, kFieldHeight);

    pairCaption.setBounds(kMargin, kPairRowY, kLabelColumn, kFieldHeight);
    pairBox.setBounds(kFieldX, kPairRowY, kFieldWidth, kFieldHeight);

    // ---- meters -------------------------------------------------------------
    // Label column and bar share the field edges exactly, so the three bars are
    // identical in x and width by construction.
    auto layoutMeter = [](LevelMeter& meter, juce::Label& label, int y)
    {
        label.setBounds(kMargin, y, kLabelColumn, kMeterRowHeight);
        meter.setBounds(kFieldX, y + (kMeterRowHeight - kMeterBarHeight) / 2,
                        kFieldWidth, kMeterBarHeight);
    };

    layoutMeter(appMeter, appMeterLabel, kMeterY);
    layoutMeter(pairMeter, pairMeterLabel, kMeterY + kMeterRowHeight);
    layoutMeter(outMeter, outMeterLabel, kMeterY + kMeterRowHeight * 2);

    // ---- footer -------------------------------------------------------------
    footer.setBounds(kMargin, kFooterY, kWidth - kMargin * 2, 24);
}

//==================================================================================================
#pragma mark Data
//==================================================================================================

void CaptureEditor::refreshApps()
{
    processes = listAudioProcesses(true);

    appBox.clear(juce::dontSendNotification);
    images.clear();

    // Item 1 is "none"; the rest map 1:1 onto `processes` starting at id 2.
    appBox.addItem(zh("不使用"), 1);

    for (size_t i = 0; i < processes.size(); ++i)
    {
        const auto& info = processes[i];
        images.push_back(toImage(info.iconPNG));

        auto name = juce::String::fromUTF8(info.name.c_str());

        if (info.audioRunning)
            name = juce::String::fromUTF8("● ") + name;

        appBox.addItem(name, static_cast<int>(i) + 2);


    }

    syncFromProcessor();
}

void CaptureEditor::syncFromProcessor()
{
    const auto pid = processor.appPID();
    const auto name = processor.appName();

    int id = 1;

    if (pid > 0)
    {
        for (size_t i = 0; i < processes.size(); ++i)
        {
            if (processes[i].pid == pid)
            {
                id = static_cast<int>(i) + 2;
                break;
            }
        }

        if (id == 1 && name.isNotEmpty())
        {
            // The saved App is not running: keep the name visible so the user
            // knows what the project expects.
            int newId = appBox.getNumItems() + 1;
            appBox.addItem(juce::String::fromUTF8("○ ") + name, newId);
            id = newId;
        }
    }

    appBox.setSelectedId(id, juce::dontSendNotification);

    const int pair = processor.selectedPair();
    pairBox.setSelectedId(processor.pairSelected() ? pair + 2 : 1, juce::dontSendNotification);
}

void CaptureEditor::updateStatus()
{
    const bool alive = processor.agentAlive();
    const auto state = processor.agentState();

    // Both sources have their own grant: the App tap needs 系统录音, the
    // virtual pair needs 麦克风. Only complain about the one in use, and only
    // when the OS says it was refused (0), not while it is undecided (1).
    const int tapPermission = processor.audioCapturePermissionRaw();
    const int micPermission = processor.microphonePermissionRaw();
    const bool pairWanted = processor.pairSelected();

    const bool needsTapPermission = processor.appSelected() && tapPermission == 0;
    const bool needsMicPermission = pairWanted && micPermission == 0;
    const bool needsPermission = alive && (needsTapPermission || needsMicPermission);

    juce::String stateText;
    juce::Colour dotColour = textTertiary;
    bool dotOn = false;

    if (! alive)
    {
        stateText = zh("后台未运行");
    }
    else if (needsPermission)
    {
        stateText = zh("需要系统权限");
        dotColour = danger;
        dotOn = true;
    }
    else if (state == State::Running)
    {
        stateText = zh("运行中");
        dotColour = positive;
        dotOn = true;
    }
    else if (state == State::Starting)
    {
        stateText = zh("连接中");
        dotColour = warning;
        dotOn = true;
    }
    else if (state == State::Error)
    {
        stateText = zh("无法采集");
        dotColour = danger;
        dotOn = true;
    }
    else
    {
        stateText = zh("待命");
        dotColour = textTertiary;
    }

    agentLabel.setText(stateText, juce::dontSendNotification);
    agentDot.setState(dotOn, dotColour);

    // Footer is a single line that changes with the situation, never a log.
    juce::String detail;

    if (! wng::systemSupportsAppCapture() && ! processor.pairSelected())
    {
        detail = zh("当前 macOS ") + juce::String(wng::systemVersionText())
                 + zh(" 不支持应用采集，请改用虚拟通道");
    }
    else if (! alive)
    {
        detail = zh("插件会自动启动后台服务，请稍候");
    }
    else if (needsPermission)
    {
        detail = needsMicPermission
                     ? zh("系统设置 → 隐私与安全性 → 麦克风 → 万能鸭录音机架")
                     : zh("系统设置 → 隐私与安全性 → 录屏与系统录音 → 万能鸭录音机架");
    }
    else if (processor.appSelected()
             && processor.tapState() == State::Waiting
             && processor.tapFault() == Fault::ProcessGone)
    {
        detail = zh("应用未在播放，重新打开后会自动连接");
    }
    else if (processor.appSelected()
             && processor.channelRate() > 0
             && processor.hostRate() > 0
             && processor.channelRate() != processor.hostRate())
    {
        detail = zh("虚拟通道 ") + juce::String(processor.channelRate()) + zh(" Hz，工程 ")
                 + juce::String(processor.hostRate()) + zh(" Hz");
    }
    else
    {
        const int ppm = processor.driftPPM();
        detail = juce::String(processor.hostRate()) + " Hz";

        if (ppm != 0)
            detail += zh("   ·   漂移补偿 ") + (ppm > 0 ? "+" : "") + juce::String(ppm) + " ppm";

        if (processor.underruns() > 0)
            detail += zh("   ·   缓冲 ") + juce::String(static_cast<int>(processor.underruns()));
    }

    footer.setText(detail, juce::dontSendNotification);
}

void CaptureEditor::timerCallback()
{
    if (++statusTicks >= 8)
    {
        statusTicks = 0;
        refreshApps();
    }

    appMeter.setLevel(processor.appSelected() ? processor.tapPeak() : 0.0f);
    pairMeter.setLevel(processor.pairSelected() ? processor.channelPeak() : 0.0f);
    outMeter.setLevel(processor.outputPeak());

    updateStatus();
}
