// Development-only: renders both editors offscreen to PNG so the layout can be
// inspected without a DAW. Not installed with the product.
#include <JuceHeader.h>

#include "CaptureEditor.h"
#include "CaptureProcessor.h"
#include "ReturnEditor.h"
#include "ReturnProcessor.h"

#include <cstdio>

namespace {

void dumpTree(juce::Component& c, int depth = 0)
{
    const auto& b = c.getBounds();

    juce::String extra;

    if (auto* label = dynamic_cast<juce::Label*>(&c))
        extra = "  font=" + juce::String(label->getFont().getHeight())
                + "  text=" + label->getText();
    else if (auto* box = dynamic_cast<juce::ComboBox*>(&c))
        extra = "  font=" + juce::String(box->getLookAndFeel().getComboBoxFont(*box).getHeight())
                + "  text=" + box->getText();

    std::printf("%*s%-28s x=%-4d y=%-4d w=%-4d h=%-4d%s%s\n", depth * 2, "",
                typeid(c).name(), b.getX(), b.getY(), b.getWidth(), b.getHeight(),
                c.isVisible() ? "" : " (hidden)", extra.toRawUTF8());

    for (auto* child : c.getChildren())
        dumpTree(*child, depth + 1);
}

void render(juce::Component& component, const juce::File& file, float scale)
{
    juce::Image image(juce::Image::ARGB,
                      static_cast<int>(component.getWidth() * scale),
                      static_cast<int>(component.getHeight() * scale),
                      true);

    juce::Graphics g(image);
    g.addTransform(juce::AffineTransform::scale(scale));
    component.paintEntireComponent(g, true);

    juce::FileOutputStream stream(file);
    juce::PNGImageFormat png;
    png.writeImageToStream(image, stream);
    std::printf("wrote %s\n", file.getFullPathName().toRawUTF8());
}

} // namespace

class PreviewApp final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "wannengya preview"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        const auto out = juce::File::getCurrentWorkingDirectory();

        {
            CaptureProcessor processor;
            processor.prepareToPlay(48000.0, 256);
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            editor->setSize(editor->getWidth(), editor->getHeight());
            std::printf("capture %dx%d\n", editor->getWidth(), editor->getHeight());
            dumpTree(*editor);
            render(*editor, out.getChildFile("capture.png"), 2.0f);
        }

        {
            ReturnProcessor processor;
            processor.prepareToPlay(48000.0, 256);
            std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
            editor->setSize(editor->getWidth(), editor->getHeight());
            std::printf("return %dx%d\n", editor->getWidth(), editor->getHeight());
            dumpTree(*editor);
            render(*editor, out.getChildFile("return.png"), 2.0f);
        }

        quit();
    }

    void shutdown() override {}
    void systemRequestedQuit() override { quit(); }
};

START_JUCE_APPLICATION(PreviewApp)
